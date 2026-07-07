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

#include "WorldSession.h"
#include "GameTime.h"
#include "HotfixPackets.h"
#include "Log.h"
#include "MapUtils.h"
#include "World.h"

namespace
{
constexpr int32 SkyridingOverrideRecordId = 2106;
constexpr int32 SkyridingOverrideSourceRecordId = 2106;
constexpr int32 SkyridingOverrideHotfixPushId = 108362;
constexpr uint32 SkyridingOverrideSpellDataTableHash = 3396722460;

bool IsSkyridingOverrideHotfix(DB2Manager::HotfixRecord const& hotfixRecord)
{
    return hotfixRecord.RecordID == SkyridingOverrideRecordId || hotfixRecord.ID.PushID == SkyridingOverrideHotfixPushId;
}

bool TryWriteSkyridingOverrideBlob(uint32 tableHash, int32 recordId, LocaleConstant locale, ByteBuffer& data, uint32& size)
{
    if (tableHash != SkyridingOverrideSpellDataTableHash || recordId != SkyridingOverrideRecordId)
        return false;

    if (std::vector<uint8> const* blobData = sDB2Manager.GetHotfixBlobData(tableHash, recordId, locale))
    {
        size = uint32(blobData->size());
        data.append(blobData->data(), blobData->size());
        return true;
    }

    return false;
}
}

void WorldSession::HandleDBQueryBulk(WorldPackets::Hotfix::DBQueryBulk& dbQuery)
{
    DB2StorageBase const* store = sDB2Manager.GetStorage(dbQuery.TableHash);
    for (WorldPackets::Hotfix::DBQueryBulk::DBQueryRecord const& record : dbQuery.Queries)
    {
        WorldPackets::Hotfix::DBReply dbReply;
        dbReply.TableHash = dbQuery.TableHash;
        dbReply.RecordID = record.RecordID;

        uint32 skyridingBlobSize = 0;
        if (TryWriteSkyridingOverrideBlob(dbQuery.TableHash, record.RecordID, GetSessionDbcLocale(), dbReply.Data, skyridingBlobSize))
        {
            dbReply.Status = DB2Manager::HotfixRecord::Status::Valid;
            dbReply.Timestamp = GameTime::GetGameTime();
        }
        else if (store && (store->HasRecord(record.RecordID) || (record.RecordID == SkyridingOverrideRecordId && store->HasRecord(SkyridingOverrideSourceRecordId))))
        {
            uint32 recordIdToWrite = record.RecordID == SkyridingOverrideRecordId && !store->HasRecord(record.RecordID) ? SkyridingOverrideSourceRecordId : record.RecordID;
            dbReply.Status = DB2Manager::HotfixRecord::Status::Valid;
            dbReply.Timestamp = GameTime::GetGameTime();
            store->WriteRecord(recordIdToWrite, GetSessionDbcLocale(), dbReply.Data);

            if (std::vector<DB2Manager::HotfixOptionalData> const* optionalDataEntries = sDB2Manager.GetHotfixOptionalData(dbQuery.TableHash, recordIdToWrite, GetSessionDbcLocale()))
            {
                for (DB2Manager::HotfixOptionalData const& optionalData : *optionalDataEntries)
                {
                    dbReply.Data << uint32(optionalData.Key);
                    dbReply.Data.append(optionalData.Data.data(), optionalData.Data.size());
                }
            }
        }
        else
        {
            TC_LOG_TRACE("network", "CMSG_DB_QUERY_BULK: {} requested non-existing entry {} in datastore: {}", GetPlayerInfo(), record.RecordID, dbQuery.TableHash);
            dbReply.Timestamp = GameTime::GetGameTime();
        }

        SendPacket(dbReply.Write());
    }
}

void WorldSession::SendAvailableHotfixes()
{
    WorldPackets::Hotfix::AvailableHotfixes availableHotfixes;
    availableHotfixes.VirtualRealmAddress = GetVirtualRealmAddress();

    for (auto const& [pushId, push] : sDB2Manager.GetHotfixData())
    {
        if (!(push.AvailableLocalesMask & (1 << GetSessionDbcLocale())))
            continue;

        availableHotfixes.Hotfixes.insert(push.Records.front().ID);

    }

    SendPacket(availableHotfixes.Write());
}

void WorldSession::HandleHotfixRequest(WorldPackets::Hotfix::HotfixRequest& hotfixQuery)
{
    DB2Manager::HotfixContainer const& hotfixes = sDB2Manager.GetHotfixData();
    WorldPackets::Hotfix::HotfixConnect hotfixQueryResponse;
    hotfixQueryResponse.Hotfixes.reserve(hotfixQuery.Hotfixes.size());
    for (int32 hotfixId : hotfixQuery.Hotfixes)
    {
        if (DB2Manager::HotfixPush const* hotfixRecords = Trinity::Containers::MapGetValuePtr(hotfixes, hotfixId))
        {
            for (DB2Manager::HotfixRecord const& hotfixRecord : hotfixRecords->Records)
            {
                if (!(hotfixRecord.AvailableLocalesMask & (1 << GetSessionDbcLocale())))
                    continue;

                WorldPackets::Hotfix::HotfixConnect::HotfixData& hotfixData = hotfixQueryResponse.Hotfixes.emplace_back();
                hotfixData.Record = hotfixRecord;
                if (hotfixRecord.HotfixStatus == DB2Manager::HotfixRecord::Status::Valid)
                {
                    DB2StorageBase const* storage = sDB2Manager.GetStorage(hotfixRecord.TableHash);
                    uint32 recordIdToWrite = uint32(hotfixRecord.RecordID);
                    if (IsSkyridingOverrideHotfix(hotfixRecord) && storage && !storage->HasRecord(recordIdToWrite) && storage->HasRecord(SkyridingOverrideSourceRecordId))
                        recordIdToWrite = SkyridingOverrideSourceRecordId;

                    bool wroteSkyridingOverrideBlob = TryWriteSkyridingOverrideBlob(hotfixRecord.TableHash, hotfixRecord.RecordID, GetSessionDbcLocale(), hotfixQueryResponse.HotfixContent, hotfixData.Size);
                    if (!wroteSkyridingOverrideBlob && storage && storage->HasRecord(recordIdToWrite))
                    {
                        std::size_t pos = hotfixQueryResponse.HotfixContent.size();
                        storage->WriteRecord(recordIdToWrite, GetSessionDbcLocale(), hotfixQueryResponse.HotfixContent);

                        if (std::vector<DB2Manager::HotfixOptionalData> const* optionalDataEntries = sDB2Manager.GetHotfixOptionalData(hotfixRecord.TableHash, recordIdToWrite, GetSessionDbcLocale()))
                        {
                            for (DB2Manager::HotfixOptionalData const& optionalData : *optionalDataEntries)
                            {
                                hotfixQueryResponse.HotfixContent << uint32(optionalData.Key);
                                hotfixQueryResponse.HotfixContent.append(optionalData.Data.data(), optionalData.Data.size());
                            }
                        }

                        hotfixData.Size = hotfixQueryResponse.HotfixContent.size() - pos;
                    }
                    else if (!wroteSkyridingOverrideBlob)
                    {
                        if (std::vector<uint8> const* blobData = sDB2Manager.GetHotfixBlobData(hotfixRecord.TableHash, hotfixRecord.RecordID, GetSessionDbcLocale()))
                        {
                            hotfixData.Size = blobData->size();
                            hotfixQueryResponse.HotfixContent.append(blobData->data(), blobData->size());
                        }
                        else
                            // Do not send Status::Valid when we don't have a hotfix blob for current locale
                            hotfixData.Record.HotfixStatus = storage ? DB2Manager::HotfixRecord::Status::RecordRemoved : DB2Manager::HotfixRecord::Status::Invalid;
                    }
                }
            }
        }
    }

    SendPacket(hotfixQueryResponse.Write());
}
