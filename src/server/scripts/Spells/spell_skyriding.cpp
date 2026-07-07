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
#include "DB2Stores.h"
#include "MovementPackets.h"
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
constexpr float SkyridingAscentGroundHorizontalSpeed = 28.0f;
constexpr float SkyridingAscentGroundVerticalSpeed = 58.0f;
constexpr float SkyridingAscentFlyingVerticalSpeed = 52.0f;
constexpr float SkyridingForwardSurgeSpeedBonus = 36.0f;
constexpr float SkyridingWhirlingSurgeSpeedBonus = 64.0f;
constexpr float SkyridingPitchVerticalVelocityScale = 0.55f;

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

void SetSkyridingPreferred(Player* player, bool preferred)
{
    if (preferred)
        SkyridingPreferredPlayers.insert(player->GetGUID());
    else
        SkyridingPreferredPlayers.erase(player->GetGUID());
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

void DisableSkyriding(Player* player, bool restoreNormalFlight, bool preserveFlightStyle = false)
{
    player->RemoveAurasDueToSpell(SPELL_SKYRIDING_KEYBOUND_AURA);
    if (!preserveFlightStyle)
        player->RemoveAurasDueToSpell(SPELL_FLIGHT_STYLE_SKYRIDING);
    ApplySkyridingTemporarySpells(player, false);

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
}

bool EnableSkyriding(Player* player)
{
    if (!player->IsAlive())
        return false;

    if (!player->IsMounted())
        return false;

    if (!sFlightCapabilityStore.HasRecord(SkyridingFlightCapabilityId))
        return false;

    player->SetFlightCapabilityID(SkyridingFlightCapabilityId, true);
    player->SetCanDoubleJump(true);
    player->SetCanFly(true);
    player->SetCanAdvFly(true);
    player->SetCanTransitionBetweenSwimAndFly(true);
    ApplyFlightStyleVisual(player, true);
    ApplySkyridingTemporarySpells(player, true);

    if (!player->HasAura(SPELL_SKYRIDING_KEYBOUND_AURA))
        player->CastSpell(player, SPELL_SKYRIDING_KEYBOUND_AURA, TRIGGERED_FULL_MASK);

    return true;
}

void EnsureSkyridingActionBar(Player* player)
{
    if (!player->HasAura(SPELL_SKYRIDING_KEYBOUND_AURA))
        player->CastSpell(player, SPELL_SKYRIDING_KEYBOUND_AURA, TRIGGERED_FULL_MASK);

    ApplySkyridingTemporarySpells(player, true);
}

SpellCastResult CheckCanUseSkyriding(Player const* player, bool allowGroundTakeoff = false)
{
    if (!player->IsMounted())
        return SPELL_FAILED_NOT_MOUNTED;

    if (!IsSkyridingEnabled(player))
        return SPELL_FAILED_NOT_FLYING;

    if (!allowGroundTakeoff && !IsPlayerAdvFlying(player))
        return SPELL_FAILED_NOT_FLYING;

    return SPELL_FAILED_SUCCESS;
}

float GetCurrentMovementPitch(Player const* player)
{
    float const pitch = player->m_movementInfo.pitch;
    return std::isfinite(pitch) ? pitch : 0.0f;
}

float GetCurrentFacingOrientation(Player const* player)
{
    float const unitOrientation = player->GetOrientation();
    float const movementOrientation = player->m_movementInfo.pos.GetOrientation();
    return std::isfinite(unitOrientation) ? unitOrientation : movementOrientation;
}

void SendSkyridingAdvFlyingAddImpulse(Player* player, float horizontalSpeed, float verticalSpeed, char const* /*source*/)
{
    if (!IsPlayerAdvFlying(player))
        return;

    float const unitOrientation = player->GetOrientation();
    float const movementOrientation = player->m_movementInfo.pos.GetOrientation();
    float const orientation = std::isfinite(movementOrientation) ? movementOrientation : unitOrientation;
    float const pitch = GetCurrentMovementPitch(player);
    float const pitchCos = std::max(std::cos(pitch), 0.0f);
    float const directionX = std::cos(orientation) * horizontalSpeed * pitchCos;
    float const directionY = std::sin(orientation) * horizontalSpeed * pitchCos;
    float const directionZ = verticalSpeed;
    uint32 const sequenceIndex = player->GetAndIncrementMovementCounter();

    WorldPackets::Movement::MoveAddImpulse packet;
    packet.MoverGUID = player->GetGUID();
    packet.SequenceIndex = sequenceIndex;
    packet.Direction = TaggedPosition<Position::XYZ>(directionX, directionY, directionZ);
    player->SendDirectMessage(packet.Write());
}

void SendSkyridingTakeoffAddImpulse(Player* player, float horizontalSpeed, float verticalSpeed, char const* /*source*/)
{
    float const orientation = GetCurrentFacingOrientation(player);
    float const directionX = std::cos(orientation) * horizontalSpeed;
    float const directionY = std::sin(orientation) * horizontalSpeed;
    float const directionZ = verticalSpeed;
    uint32 const sequenceIndex = player->GetAndIncrementMovementCounter();

    WorldPackets::Movement::MoveAddImpulse packet;
    packet.MoverGUID = player->GetGUID();
    packet.SequenceIndex = sequenceIndex;
    packet.Direction = TaggedPosition<Position::XYZ>(directionX, directionY, directionZ);
    player->SendDirectMessage(packet.Write());
}

void SendSkyridingImpulse(Player* player, float horizontalSpeed, float verticalSpeed, char const* source)
{
    SendSkyridingAdvFlyingAddImpulse(player, horizontalSpeed, verticalSpeed, source);
}

void SendSkyridingTakeoffImpulse(Player* player, float horizontalSpeed, float verticalSpeed, char const* source)
{
    SendSkyridingTakeoffAddImpulse(player, horizontalSpeed, verticalSpeed, source);
}

void SendSkyridingPreservedForwardImpulse(Player* player, char const* spellName, float horizontalBonus, float verticalBonus)
{
    float const pitch = GetCurrentMovementPitch(player);
    float const pitchVerticalBonus = verticalBonus <= 0.0f ? horizontalBonus * std::sin(pitch) * SkyridingPitchVerticalVelocityScale : 0.0f;
    float const verticalImpulse = verticalBonus > 0.0f ? verticalBonus : pitchVerticalBonus;

    SendSkyridingImpulse(player, horizontalBonus, verticalImpulse, spellName);
}

void SendSkyridingForwardImpulse(Player* player, char const* spellName, float horizontalSpeed, float verticalSpeed)
{
    SendSkyridingPreservedForwardImpulse(player, spellName, horizontalSpeed, verticalSpeed);
}

void SendSkyridingAscentImpulse(Player* player, char const* spellName, float horizontalSpeed, float groundVerticalSpeed, float flyingVerticalSpeed)
{
    if (!IsPlayerAdvFlying(player))
    {
        SendSkyridingTakeoffImpulse(player, horizontalSpeed, groundVerticalSpeed, spellName);
        return;
    }

    SendSkyridingPreservedForwardImpulse(player, spellName, horizontalSpeed, flyingVerticalSpeed);
}

void SendSkyridingWhirlingImpulse(Player* player, char const* spellName, float horizontalSpeed, float verticalSpeed)
{
    SendSkyridingPreservedForwardImpulse(player, spellName, horizontalSpeed, verticalSpeed);
}

void SendSkyridingAirBrake(Player* player)
{
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

        if (!player->IsAlive())
            return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;

        if (player->IsMounted())
            return SPELL_FAILED_NOT_HERE;

        if (!sFlightCapabilityStore.HasRecord(SkyridingFlightCapabilityId))
            return SPELL_FAILED_NOT_HERE;

        return SPELL_FAILED_SUCCESS;
    }

    void ToggleFlightStyle()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return;

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

        return CheckCanUseSkyriding(player, _kind == ImpulseKind::Ascent);
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

        for (uint32 spellId : SkyridingSecondWindTargets)
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId, player->GetMap()->GetDifficultyID());
            if (!spellInfo)
                continue;

            // Second Wind's DB2 effect already restores the skyriding charge category once.
            // This script only bridges skyriding buttons implemented as ordinary cooldowns.
            if (!spellInfo->ChargeCategoryId)
                player->GetSpellHistory()->ResetCooldown(spellId, true);
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
        if (!player->HasSpell(SPELL_SKYRIDING_TOGGLE))
            player->LearnSpell(SPELL_SKYRIDING_TOGGLE, false);

        if (!player->HasSpell(SPELL_SKYRIDING_BASICS))
            player->LearnSpell(SPELL_SKYRIDING_BASICS, false);

        bool const preferSkyriding = player->HasAura(SPELL_FLIGHT_STYLE_SKYRIDING) || (IsSkyridingEnabled(player) && !player->HasAura(SPELL_FLIGHT_STYLE_STEADY));
        RestoreSkyridingPreferred(player, preferSkyriding);
        DisableSkyriding(player, false, true);
        ApplyFlightStyleVisual(player, preferSkyriding);
    }

    void OnLogout(Player* player) override
    {
        DisableSkyriding(player, false, true);
    }

    void OnPlayerRepop(Player* player) override
    {
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
                    DisableSkyriding(player, false, true);
                    continue;
                }

                if (!player->IsAlive())
                    continue;

                if (!player->IsMounted())
                {
                    if (player->HasAura(SPELL_SKYRIDING_KEYBOUND_AURA) || HasAnySkyridingTemporarySpell(player))
                        DisableSkyriding(player, false, true);

                    ApplyFlightStyleVisual(player, IsSkyridingPreferred(player));

                    continue;
                }

                if (IsSkyridingPreferred(player))
                {
                    if (!IsSkyridingEnabled(player))
                        EnableSkyriding(player);
                    else
                        EnsureSkyridingActionBar(player);
                }
                else if (IsSkyridingEnabled(player))
                {
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
    RegisterSpellScriptWithArgs(spell_skyriding_impulse, "spell_skyriding_forward_surge", "forward-surge", SkyridingForwardSurgeSpeedBonus, 0.0f, spell_skyriding_impulse::ImpulseKind::Forward);
    RegisterSpellScriptWithArgs(spell_skyriding_impulse, "spell_skyriding_skyward_ascent", "skyward-ascent", SkyridingAscentGroundHorizontalSpeed, SkyridingAscentGroundVerticalSpeed, spell_skyriding_impulse::ImpulseKind::Ascent);
    RegisterSpellScriptWithArgs(spell_skyriding_impulse, "spell_skyriding_keybound_ascent", "keybound-ascent", SkyridingAscentGroundHorizontalSpeed, SkyridingAscentGroundVerticalSpeed, spell_skyriding_impulse::ImpulseKind::Ascent);
    RegisterSpellScriptWithArgs(spell_skyriding_impulse, "spell_skyriding_whirling_surge", "whirling-surge", SkyridingWhirlingSurgeSpeedBonus, 0.0f, spell_skyriding_impulse::ImpulseKind::Whirling);
    RegisterSpellScript(spell_skyriding_aerial_stop);
    RegisterSpellScript(spell_skyriding_second_wind);
    new skyriding_player_cleanup();
    new skyriding_world_cleanup();
}
