/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptMgr.h"
#include "Log.h"
#include "DB2Stores.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Position.h"
#include "Spell.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "World.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <shared_mutex>
#include <unordered_set>
#include <utility>

namespace
{
enum SkyridingSpells : uint32
{
    SPELL_SKYRIDING_BASICS             = 376777, // Skyriding Basics
    SPELL_SKYRIDING_TOGGLE             = 436854, // Switch Flight Style
    SPELL_SKYRIDING_FORWARD_SURGE      = 372608,
    SPELL_SKYRIDING_SKYWARD_ASCENT     = 372610,
    SPELL_SKYRIDING_WHIRLING_SURGE     = 361584,
    SPELL_SKYRIDING_AERIAL_STOP        = 403092,
    SPELL_SKYRIDING_SECOND_WIND        = 425782,
    SPELL_SKYRIDING_SECOND_WIND_LEGACY = 425623,
    SPELL_SKYRIDING_SECOND_WIND_PROC   = 425625,
    SPELL_SKYRIDING_KEYBOUND_ASCENT    = 374763,
    SPELL_SKYRIDING_KEYBOUND_AURA      = 406095,
    SPELL_FLIGHT_STYLE_SKYRIDING       = 404464, // Flight Style: Skyriding
    SPELL_FLIGHT_STYLE_STEADY          = 404468  // Flight Style: Steady
};

constexpr uint32 SkyridingOverrideSpellDataId = 2106;
constexpr uint32 SkyridingFlightCapabilityId = 1;
constexpr uint32 SkyridingCleanupIntervalMs = 1000;
constexpr float SkyridingMovingForwardVelocity = 5.0f;
constexpr float SkyridingAscentGroundHorizontalSpeed = 28.0f;
constexpr float SkyridingAscentGroundVerticalSpeed = 58.0f;
constexpr float SkyridingAscentFlyingVerticalSpeed = 44.0f;

std::array<uint32, 5> constexpr SkyridingTemporarySpells =
{
    SPELL_SKYRIDING_FORWARD_SURGE,
    SPELL_SKYRIDING_WHIRLING_SURGE,
    SPELL_SKYRIDING_SKYWARD_ASCENT,
    SPELL_SKYRIDING_AERIAL_STOP,
    SPELL_SKYRIDING_SECOND_WIND
};

std::array<uint32, 4> constexpr SkyridingSecondWindTargets =
{
    SPELL_SKYRIDING_FORWARD_SURGE,
    SPELL_SKYRIDING_WHIRLING_SURGE,
    SPELL_SKYRIDING_SKYWARD_ASCENT,
    SPELL_SKYRIDING_AERIAL_STOP
};

std::unordered_set<ObjectGuid> SkyridingPreferredPlayers;
std::unordered_set<ObjectGuid> SkyridingActionBarLoggedPlayers;

bool IsSkyridingEnabled(Player const* player)
{
    return player->HasExtraUnitMovementFlag2(MOVEMENTFLAG3_CAN_ADV_FLY);
}

bool IsPlayerAdvFlying(Player const* player)
{
    return player->HasExtraUnitMovementFlag2(MOVEMENTFLAG3_ADV_FLYING) && player->m_movementInfo.advFlying.has_value();
}

bool IsSkyridingPreferred(Player const* player)
{
    return SkyridingPreferredPlayers.find(player->GetGUID()) != SkyridingPreferredPlayers.end() || player->HasAura(SPELL_FLIGHT_STYLE_SKYRIDING);
}

void LogSkyridingState(Player const* player, char const* stage, char const* detail = "")
{
    if (!player)
    {
        TC_LOG_INFO("server", "Skyriding: {} player=<none> detail={}", stage, detail);
        return;
    }

    TC_LOG_INFO("server", "Skyriding: {} player={} guid={} knownToggle={} mounted={} alive={} preferred={} advFly={} flightCapability={} dhDoubleJumpAura={} detail={}",
        stage, player->GetName(), player->GetGUID().ToString(), player->HasSpell(SPELL_SKYRIDING_TOGGLE), player->IsMounted(),
        player->IsAlive(), IsSkyridingPreferred(player), IsSkyridingEnabled(player), player->GetFlightCapabilityID(), player->HasAura(SPELL_DH_DOUBLE_JUMP), detail);
}

void SetSkyridingPreferred(Player* player, bool preferred)
{
    if (preferred)
        SkyridingPreferredPlayers.insert(player->GetGUID());
    else
        SkyridingPreferredPlayers.erase(player->GetGUID());

    LogSkyridingState(player, preferred ? "preference-set-skyriding" : "preference-set-stable");
}

void RestoreSkyridingPreferred(Player* player, bool preferred)
{
    if (preferred)
        SkyridingPreferredPlayers.insert(player->GetGUID());
    else
        SkyridingPreferredPlayers.erase(player->GetGUID());
}

void ApplyFlightStyleVisual(Player* player, bool skyriding)
{
    if (skyriding)
    {
        player->RemoveAurasDueToSpell(SPELL_FLIGHT_STYLE_STEADY);
        if (!player->HasAura(SPELL_FLIGHT_STYLE_SKYRIDING))
            player->CastSpell(player, SPELL_FLIGHT_STYLE_SKYRIDING, TRIGGERED_FULL_MASK);
    }
    else
    {
        player->RemoveAurasDueToSpell(SPELL_FLIGHT_STYLE_SKYRIDING);
        if (!player->HasAura(SPELL_FLIGHT_STYLE_STEADY))
            player->CastSpell(player, SPELL_FLIGHT_STYLE_STEADY, TRIGGERED_FULL_MASK);
    }
}

void RefreshOrdinaryFlightSpeeds(Player* player)
{
    player->UpdateSpeed(MOVE_RUN);
    player->UpdateSpeed(MOVE_FLIGHT);
    player->UpdateSpeed(MOVE_FLIGHT_BACK);
}

void ApplySkyridingTemporarySpells(Player* player, bool apply)
{
    player->SetOverrideSpellsId(apply ? int32(SkyridingOverrideSpellDataId) : 0);

    for (uint32 spellId : SkyridingTemporarySpells)
    {
        if (apply)
            player->AddTemporarySpell(spellId);
        else
            player->RemoveTemporarySpell(spellId);
    }
}

bool HasAllSkyridingTemporarySpells(Player const* player)
{
    for (uint32 spellId : SkyridingTemporarySpells)
        if (!player->HasSpell(spellId))
            return false;

    return true;
}

bool HasAnySkyridingTemporarySpell(Player const* player)
{
    for (uint32 spellId : SkyridingTemporarySpells)
        if (player->HasSpell(spellId))
            return true;

    return false;
}

void LogSkyridingActionBarState(Player const* player, char const* stage, int32 targetOverrideSpellsId)
{
    TC_LOG_INFO("server", "Skyriding: actionbar-state stage={} player={} guid={} targetOverrideSpellsId={} keyboundAura={} flightStyleSkyridingAura={} flightStyleSteadyAura={} allTemporarySpells={} forwardSurge={} whirlingSurge={} skywardAscent={} secondWind={} aerialStop={}",
        stage, player->GetName(), player->GetGUID().ToString(), targetOverrideSpellsId, player->HasAura(SPELL_SKYRIDING_KEYBOUND_AURA),
        player->HasAura(SPELL_FLIGHT_STYLE_SKYRIDING), player->HasAura(SPELL_FLIGHT_STYLE_STEADY), HasAllSkyridingTemporarySpells(player),
        player->HasSpell(SPELL_SKYRIDING_FORWARD_SURGE), player->HasSpell(SPELL_SKYRIDING_WHIRLING_SURGE),
        player->HasSpell(SPELL_SKYRIDING_SKYWARD_ASCENT), player->HasSpell(SPELL_SKYRIDING_SECOND_WIND), player->HasSpell(SPELL_SKYRIDING_AERIAL_STOP));
}

void SetSkyridingTemporarySpells(Player* player, bool apply)
{
    LogSkyridingState(player, apply ? "temporary-spells-apply" : "temporary-spells-remove");
    ApplySkyridingTemporarySpells(player, apply);
    LogSkyridingActionBarState(player, apply ? "temporary-spells-apply" : "temporary-spells-remove", apply ? int32(SkyridingOverrideSpellDataId) : 0);
}

void DisableSkyriding(Player* player, bool restoreNormalFlight, bool preserveFlightStyle = false)
{
    LogSkyridingState(player, restoreNormalFlight ? "disable-begin-restore-normal-flight" : "disable-begin-cleanup");
    player->RemoveAurasDueToSpell(SPELL_SKYRIDING_KEYBOUND_AURA);
    if (!preserveFlightStyle)
        player->RemoveAurasDueToSpell(SPELL_FLIGHT_STYLE_SKYRIDING);
    SetSkyridingTemporarySpells(player, false);
    SkyridingActionBarLoggedPlayers.erase(player->GetGUID());

    bool const keepOrdinaryFlight = restoreNormalFlight && player->IsAlive() && player->IsMounted();

    player->SetCanAdvFly(false);
    if (!keepOrdinaryFlight)
        player->SetFlightCapabilityID(0, true);

    player->SetCanDoubleJump(player->HasAura(SPELL_DH_DOUBLE_JUMP));

    player->SetCanFly(keepOrdinaryFlight);
    player->SetCanTransitionBetweenSwimAndFly(keepOrdinaryFlight);

    if (keepOrdinaryFlight)
    {
        ApplyFlightStyleVisual(player, false);
        RefreshOrdinaryFlightSpeeds(player);
    }

    LogSkyridingState(player, keepOrdinaryFlight ? "disable-end-normal-flight-restored" : "disable-end-no-normal-flight");
}

bool EnableSkyriding(Player* player)
{
    LogSkyridingState(player, "enable-begin");

    if (!player->IsAlive())
    {
        LogSkyridingState(player, "enable-failed", "player-not-alive");
        return false;
    }

    if (!player->IsMounted())
    {
        LogSkyridingState(player, "enable-failed", "player-not-mounted");
        return false;
    }

    if (!sFlightCapabilityStore.HasRecord(SkyridingFlightCapabilityId))
    {
        LogSkyridingState(player, "enable-failed", "missing-flight-capability");
        return false;
    }

    player->SetFlightCapabilityID(SkyridingFlightCapabilityId, true);
    player->SetCanDoubleJump(true);
    player->SetCanFly(true);
    player->SetCanAdvFly(true);
    player->SetCanTransitionBetweenSwimAndFly(true);
    ApplyFlightStyleVisual(player, true);
    SetSkyridingTemporarySpells(player, true);

    if (!player->HasAura(SPELL_SKYRIDING_KEYBOUND_AURA))
    {
        LogSkyridingState(player, "enable-cast-keybound-aura");
        player->CastSpell(player, SPELL_SKYRIDING_KEYBOUND_AURA, TRIGGERED_FULL_MASK);
    }

    LogSkyridingState(player, "enable-end");
    return true;
}

void EnsureSkyridingActionBar(Player* player)
{
    bool const needsHeal = !player->HasAura(SPELL_SKYRIDING_KEYBOUND_AURA) || !HasAllSkyridingTemporarySpells(player);
    bool const logThisSync = needsHeal || SkyridingActionBarLoggedPlayers.insert(player->GetGUID()).second;

    if (!player->HasAura(SPELL_SKYRIDING_KEYBOUND_AURA))
    {
        LogSkyridingState(player, "actionbar-heal-keybound-aura");
        player->CastSpell(player, SPELL_SKYRIDING_KEYBOUND_AURA, TRIGGERED_FULL_MASK);
    }

    ApplySkyridingTemporarySpells(player, true);

    if (logThisSync)
        LogSkyridingActionBarState(player, needsHeal ? "actionbar-heal" : "actionbar-ensure", int32(SkyridingOverrideSpellDataId));
}

SpellCastResult CheckCanUseSkyriding(Player const* player)
{
    if (!player->IsMounted())
    {
        LogSkyridingState(player, "active-spell-check-failed", "player-not-mounted");
        return SPELL_FAILED_NOT_MOUNTED;
    }

    if (!IsSkyridingEnabled(player))
    {
        LogSkyridingState(player, "active-spell-check-failed", "skyriding-not-enabled");
        return SPELL_FAILED_NOT_FLYING;
    }

    LogSkyridingState(player, "active-spell-check-ok");
    return SPELL_FAILED_SUCCESS;
}

void SendSkyridingImpulse(Player* player, float horizontalSpeed, float verticalSpeed)
{
    float const unitOrientation = player->GetOrientation();
    float const movementOrientation = player->m_movementInfo.pos.GetOrientation();
    float const orientation = std::isfinite(movementOrientation) ? movementOrientation : unitOrientation;

    TC_LOG_INFO("server", "Skyriding: impulse player={} guid={} horizontalSpeed={} verticalSpeed={} unitOrientation={} movementOrientation={} usedOrientation={}",
        player->GetName(), player->GetGUID().ToString(), horizontalSpeed, verticalSpeed, unitOrientation, movementOrientation, orientation);

    Position origin = player->GetPosition();
    origin.Relocate(origin.GetPositionX() - std::cos(orientation), origin.GetPositionY() - std::sin(orientation), origin.GetPositionZ(), orientation);
    player->KnockbackFrom(origin, horizontalSpeed, verticalSpeed);
}

bool ShouldSendSkyridingImpulse(Player const* player, char const* spellName, float horizontalSpeed, float verticalSpeed)
{
    if (!IsPlayerAdvFlying(player))
        return true;

    MovementInfo::AdvFlying const& advFlying = *player->m_movementInfo.advFlying;
    float const forwardVelocity = std::fabs(advFlying.forwardVelocity);
    float const upVelocity = advFlying.upVelocity;

    if (horizontalSpeed > 0.0f && forwardVelocity >= horizontalSpeed)
    {
        TC_LOG_INFO("server", "Skyriding: impulse-skip-preserve-forward player={} guid={} spell={} forwardVelocity={} requestedHorizontalSpeed={} upVelocity={}",
            player->GetName(), player->GetGUID().ToString(), spellName, forwardVelocity, horizontalSpeed, upVelocity);
        return false;
    }

    if (verticalSpeed > 0.0f && upVelocity >= verticalSpeed)
    {
        TC_LOG_INFO("server", "Skyriding: impulse-skip-preserve-up player={} guid={} spell={} forwardVelocity={} upVelocity={} requestedVerticalSpeed={}",
            player->GetName(), player->GetGUID().ToString(), spellName, forwardVelocity, upVelocity, verticalSpeed);
        return false;
    }

    return true;
}

void SendSkyridingForwardImpulse(Player* player, char const* spellName, float horizontalSpeed, float verticalSpeed)
{
    LogSkyridingState(player, "active-spell-impulse", spellName);
    if (!ShouldSendSkyridingImpulse(player, spellName, horizontalSpeed, verticalSpeed))
        return;

    SendSkyridingImpulse(player, horizontalSpeed, verticalSpeed);
}

void SendSkyridingAscentImpulse(Player* player, char const* spellName, float horizontalSpeed, float groundVerticalSpeed, float flyingVerticalSpeed)
{
    LogSkyridingState(player, "active-spell-impulse", spellName);

    if (!IsPlayerAdvFlying(player))
    {
        SendSkyridingImpulse(player, horizontalSpeed, groundVerticalSpeed);
        return;
    }

    MovementInfo::AdvFlying const& advFlying = *player->m_movementInfo.advFlying;
    float const forwardVelocity = std::fabs(advFlying.forwardVelocity);
    float const upVelocity = advFlying.upVelocity;

    if (upVelocity >= flyingVerticalSpeed)
    {
        TC_LOG_INFO("server", "Skyriding: impulse-skip-preserve-up player={} guid={} spell={} forwardVelocity={} upVelocity={} requestedVerticalSpeed={}",
            player->GetName(), player->GetGUID().ToString(), spellName, forwardVelocity, upVelocity, flyingVerticalSpeed);
        return;
    }

    SendSkyridingImpulse(player, std::max(forwardVelocity, horizontalSpeed), flyingVerticalSpeed);
}

void SendSkyridingWhirlingImpulse(Player* player, char const* spellName, float horizontalSpeed, float verticalSpeed)
{
    LogSkyridingState(player, "active-spell-impulse", spellName);

    if (IsPlayerAdvFlying(player))
    {
        MovementInfo::AdvFlying const& advFlying = *player->m_movementInfo.advFlying;
        float const forwardVelocity = std::fabs(advFlying.forwardVelocity);
        if (forwardVelocity >= SkyridingMovingForwardVelocity)
        {
            TC_LOG_INFO("server", "Skyriding: impulse-skip-preserve-whirling player={} guid={} spell={} forwardVelocity={} requestedHorizontalSpeed={} upVelocity={}",
                player->GetName(), player->GetGUID().ToString(), spellName, forwardVelocity, horizontalSpeed, advFlying.upVelocity);
            return;
        }
    }

    if (!ShouldSendSkyridingImpulse(player, spellName, horizontalSpeed, verticalSpeed))
        return;

    SendSkyridingImpulse(player, horizontalSpeed, verticalSpeed);
}

void SendSkyridingAirBrake(Player* player)
{
    LogSkyridingState(player, "active-spell-air-brake", "aerial-stop");

    float const orientation = player->GetOrientation();
    Position origin = player->GetPosition();
    origin.Relocate(origin.GetPositionX() + std::cos(orientation), origin.GetPositionY() + std::sin(orientation), origin.GetPositionZ(), orientation);
    player->KnockbackFrom(origin, 6.0f, 1.0f);
}
}

// 436854 - Switch Flight Style
class spell_skyriding_toggle : public SpellScript
{
    SpellCastResult CheckCast()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return SPELL_FAILED_BAD_TARGETS;

        LogSkyridingState(player, "toggle-check-begin");

        if (!player->IsAlive())
        {
            LogSkyridingState(player, "toggle-check-failed", "player-not-alive");
            return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;
        }

        if (player->IsMounted())
        {
            LogSkyridingState(player, "toggle-check-failed", "player-mounted");
            return SPELL_FAILED_NOT_HERE;
        }

        if (!sFlightCapabilityStore.HasRecord(SkyridingFlightCapabilityId))
        {
            LogSkyridingState(player, "toggle-check-failed", "missing-flight-capability");
            return SPELL_FAILED_NOT_HERE;
        }

        LogSkyridingState(player, "toggle-check-ok", IsSkyridingPreferred(player) ? "will-set-stable" : "will-set-skyriding");
        return SPELL_FAILED_SUCCESS;
    }

    void ToggleFlightStyle()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return;

        LogSkyridingState(player, "toggle-cast");

        bool const preferSkyriding = !IsSkyridingPreferred(player);
        SetSkyridingPreferred(player, preferSkyriding);
        ApplyFlightStyleVisual(player, preferSkyriding);

        if (!preferSkyriding)
            DisableSkyriding(player, false);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_skyriding_toggle::CheckCast);
        AfterCast += SpellCastFn(spell_skyriding_toggle::ToggleFlightStyle);
    }
};

class spell_skyriding_impulse : public SpellScript
{
public:
    enum class ImpulseKind
    {
        Forward,
        Ascent,
        Whirling
    };

    spell_skyriding_impulse(std::string spellName, float horizontalSpeed, float verticalSpeed, ImpulseKind kind) :
        _spellName(std::move(spellName)), _horizontalSpeed(horizontalSpeed), _verticalSpeed(verticalSpeed), _kind(kind) { }

private:
    SpellCastResult CheckCast()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return SPELL_FAILED_BAD_TARGETS;

        return CheckCanUseSkyriding(player);
    }

    void Launch()
    {
        if (Player* player = GetCaster()->ToPlayer())
        {
            switch (_kind)
            {
                case ImpulseKind::Ascent:
                    SendSkyridingAscentImpulse(player, _spellName.c_str(), _horizontalSpeed, _verticalSpeed, SkyridingAscentFlyingVerticalSpeed);
                    break;
                case ImpulseKind::Whirling:
                    SendSkyridingWhirlingImpulse(player, _spellName.c_str(), _horizontalSpeed, _verticalSpeed);
                    break;
                case ImpulseKind::Forward:
                    SendSkyridingForwardImpulse(player, _spellName.c_str(), _horizontalSpeed, _verticalSpeed);
                    break;
            }
        }
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_skyriding_impulse::CheckCast);
        AfterCast += SpellCastFn(spell_skyriding_impulse::Launch);
    }

    std::string _spellName;
    float _horizontalSpeed;
    float _verticalSpeed;
    ImpulseKind _kind;
};

class spell_skyriding_aerial_stop : public SpellScript
{
    SpellCastResult CheckCast()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return SPELL_FAILED_BAD_TARGETS;

        return CheckCanUseSkyriding(player);
    }

    void Stop()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return;

        SendSkyridingAirBrake(player);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_skyriding_aerial_stop::CheckCast);
        AfterCast += SpellCastFn(spell_skyriding_aerial_stop::Stop);
    }
};

// 425782 - Second Wind
class spell_skyriding_second_wind : public SpellScript
{
    SpellCastResult CheckCast()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return SPELL_FAILED_BAD_TARGETS;

        return CheckCanUseSkyriding(player);
    }

    void RestoreSkyridingCharges()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return;

        LogSkyridingState(player, "second-wind-restore");
        for (uint32 spellId : SkyridingSecondWindTargets)
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId, player->GetMap()->GetDifficultyID());
            if (!spellInfo)
                continue;

            player->GetSpellHistory()->ResetCooldown(spellId, true);
            if (spellInfo->ChargeCategoryId)
                player->GetSpellHistory()->RestoreCharge(spellInfo->ChargeCategoryId);
        }
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_skyriding_second_wind::CheckCast);
        AfterCast += SpellCastFn(spell_skyriding_second_wind::RestoreSkyridingCharges);
    }
};

class skyriding_player_cleanup : public PlayerScript
{
public:
    skyriding_player_cleanup() : PlayerScript("skyriding_player_cleanup") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        LogSkyridingState(player, "login-begin");

        if (!player->HasSpell(SPELL_SKYRIDING_TOGGLE))
        {
            LogSkyridingState(player, "login-learn-toggle");
            player->LearnSpell(SPELL_SKYRIDING_TOGGLE, false);
        }

        if (!player->HasSpell(SPELL_SKYRIDING_BASICS))
        {
            LogSkyridingState(player, "login-learn-basics");
            player->LearnSpell(SPELL_SKYRIDING_BASICS, false);
        }

        bool const preferSkyriding = player->HasAura(SPELL_FLIGHT_STYLE_SKYRIDING) || (IsSkyridingEnabled(player) && !player->HasAura(SPELL_FLIGHT_STYLE_STEADY));
        RestoreSkyridingPreferred(player, preferSkyriding);
        DisableSkyriding(player, false, true);
        ApplyFlightStyleVisual(player, preferSkyriding);
        LogSkyridingState(player, "login-end");
    }

    void OnLogout(Player* player) override
    {
        LogSkyridingState(player, "logout-cleanup");
        DisableSkyriding(player, false, true);
    }

    void OnPlayerRepop(Player* player) override
    {
        LogSkyridingState(player, "repop-cleanup");
        DisableSkyriding(player, false, true);
    }
};

class skyriding_world_cleanup : public WorldScript
{
public:
    skyriding_world_cleanup() : WorldScript("skyriding_world_cleanup") { }

    void OnUpdate(uint32 diff) override
    {
        if (_timer <= diff)
        {
            _timer = SkyridingCleanupIntervalMs;

            std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
            for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            {
                if (IsSkyridingEnabled(player) && (!player->IsAlive() || !player->IsMounted()))
                {
                    LogSkyridingState(player, "world-cleanup", !player->IsAlive() ? "player-not-alive" : "player-not-mounted");
                    DisableSkyriding(player, false, true);
                    continue;
                }

                if (!player->IsAlive())
                    continue;

                if (!player->IsMounted())
                {
                    if (player->HasAura(SPELL_SKYRIDING_KEYBOUND_AURA) || HasAnySkyridingTemporarySpell(player))
                    {
                        LogSkyridingState(player, "world-cleanup", player->HasAura(SPELL_SKYRIDING_KEYBOUND_AURA) ? "player-not-mounted-keybound-aura" : "player-not-mounted-temporary-spells");
                        DisableSkyriding(player, false, true);
                    }

                    ApplyFlightStyleVisual(player, IsSkyridingPreferred(player));

                    continue;
                }

                if (IsSkyridingPreferred(player))
                {
                    if (!IsSkyridingEnabled(player))
                    {
                        LogSkyridingState(player, "world-apply-preferred-skyriding");
                        EnableSkyriding(player);
                    }
                    else
                        EnsureSkyridingActionBar(player);
                }
                else if (IsSkyridingEnabled(player))
                {
                    LogSkyridingState(player, "world-apply-preferred-stable");
                    DisableSkyriding(player, true);
                }
            }
        }
        else
            _timer -= diff;
    }

private:
    uint32 _timer = SkyridingCleanupIntervalMs;
};

void AddSC_skyriding_spell_scripts()
{
    RegisterSpellScript(spell_skyriding_toggle);
    RegisterSpellScriptWithArgs(spell_skyriding_impulse, "spell_skyriding_forward_surge", "forward-surge", 96.0f, 0.0f, spell_skyriding_impulse::ImpulseKind::Forward);
    RegisterSpellScriptWithArgs(spell_skyriding_impulse, "spell_skyriding_skyward_ascent", "skyward-ascent", SkyridingAscentGroundHorizontalSpeed, SkyridingAscentGroundVerticalSpeed, spell_skyriding_impulse::ImpulseKind::Ascent);
    RegisterSpellScriptWithArgs(spell_skyriding_impulse, "spell_skyriding_keybound_ascent", "keybound-ascent", SkyridingAscentGroundHorizontalSpeed, SkyridingAscentGroundVerticalSpeed, spell_skyriding_impulse::ImpulseKind::Ascent);
    RegisterSpellScriptWithArgs(spell_skyriding_impulse, "spell_skyriding_whirling_surge", "whirling-surge", 220.0f, 0.0f, spell_skyriding_impulse::ImpulseKind::Whirling);
    RegisterSpellScript(spell_skyriding_aerial_stop);
    RegisterSpellScript(spell_skyriding_second_wind);
    new skyriding_player_cleanup();
    new skyriding_world_cleanup();
}
