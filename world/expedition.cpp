/**
 * EQEmulator: Everquest Server Emulator
 * Copyright (C) 2001-2020 EQEmulator Development Team (https://github.com/EQEmu/Server)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY except by those people which sell it, which
 * are required to give you total support for your newly bought product;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include "expedition.h"
#include "clientlist.h"
#include "cliententry.h"
#include "zonelist.h"
#include "zoneserver.h"
#include "worlddb.h"
#include "../common/servertalk.h"
#include "../common/string_util.h"

ExpeditionCache expedition_cache;

extern ClientList client_list;
extern ZSList zoneserver_list;

Expedition::Expedition(
	uint32_t expedition_id, uint32_t instance_id, uint32_t dz_zone_id,
	uint32_t start_time, uint32_t duration
) :
	m_expedition_id(expedition_id),
	m_dz_instance_id(instance_id),
	m_dz_zone_id(dz_zone_id),
	m_start_time(std::chrono::system_clock::from_time_t(start_time)),
	m_duration(duration)
{
	m_expire_time = m_start_time + m_duration;
}

void Expedition::SendZonesExpeditionDeleted()
{
	uint32_t pack_size = sizeof(ServerExpeditionID_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(ServerOP_ExpeditionDeleted, pack_size));
	auto buf = reinterpret_cast<ServerExpeditionID_Struct*>(pack->pBuffer);
	buf->expedition_id = GetID();
	zoneserver_list.SendPacket(pack.get());
}

void Expedition::SendZonesDurationUpdate()
{
	uint32_t packsize = sizeof(ServerExpeditionUpdateDuration_Struct);
	auto pack = std::unique_ptr<ServerPacket>(new ServerPacket(ServerOP_ExpeditionDzDuration, packsize));
	auto packbuf = reinterpret_cast<ServerExpeditionUpdateDuration_Struct*>(pack->pBuffer);
	packbuf->expedition_id = GetID();
	packbuf->new_duration_seconds = static_cast<uint32_t>(m_duration.count());
	zoneserver_list.SendPacket(pack.get());
}

void Expedition::UpdateDzSecondsRemaining(uint32_t seconds_remaining)
{
	auto now = std::chrono::system_clock::now();
	auto update_time = std::chrono::seconds(seconds_remaining);

	auto current_remaining = m_expire_time - now;
	if (current_remaining > update_time) // reduce only
	{
		LogExpeditionsDetail(
			"Updating expedition [{}] dz instance [{}] seconds remaining to [{}]s",
			GetID(), GetInstanceID(), seconds_remaining
		);

		// preserve original start time and adjust duration instead
		m_expire_time = now + update_time;
		m_duration = std::chrono::duration_cast<std::chrono::seconds>(m_expire_time - m_start_time);

		ExpeditionDatabase::UpdateDzDuration(GetInstanceID(), static_cast<uint32_t>(m_duration.count()));

		// update zone level caches and update the actual dz instance's timer
		SendZonesDurationUpdate();
	}
}

void ExpeditionCache::LoadActiveExpeditions()
{
	BenchTimer benchmark;

	m_expeditions = ExpeditionDatabase::LoadExpeditions();

	auto elapsed = benchmark.elapsed();
	LogExpeditions("World caching [{}] expeditions took {}s", m_expeditions.size(), elapsed);
}

void ExpeditionCache::AddExpedition(uint32_t expedition_id)
{
	if (expedition_id == 0)
	{
		return;
	}

	auto expedition = ExpeditionDatabase::LoadExpedition(expedition_id);

	if (expedition.GetID() == expedition_id)
	{
		auto it = std::find_if(m_expeditions.begin(), m_expeditions.end(), [&](const Expedition& expedition) {
			return expedition.GetID() == expedition_id;
		});

		if (it == m_expeditions.end())
		{
			m_expeditions.emplace_back(expedition);
		}
	}
}

void ExpeditionCache::RemoveExpedition(uint32_t expedition_id)
{
	m_expeditions.erase(std::remove_if(m_expeditions.begin(), m_expeditions.end(),
		[&](const Expedition& expedition) {
			return expedition.GetID() == expedition_id;
		}
	), m_expeditions.end());
}

void ExpeditionCache::MemberChange(uint32_t expedition_id, uint32_t character_id, bool remove)
{
	auto it = std::find_if(m_expeditions.begin(), m_expeditions.end(), [&](const Expedition& expedition) {
		return expedition.GetID() == expedition_id;
	});

	if (it != m_expeditions.end())
	{
		if (remove) {
			it->RemoveMember(character_id);
		} else {
			it->AddMember(character_id);
		}
	}
}

void ExpeditionCache::RemoveAllMembers(uint32_t expedition_id)
{
	auto it = std::find_if(m_expeditions.begin(), m_expeditions.end(), [&](const Expedition& expedition) {
		return expedition.GetID() == expedition_id;
	});

	if (it != m_expeditions.end())
	{
		it->RemoveAllMembers();
	}
}

void ExpeditionCache::Process()
{
	if (!m_process_throttle_timer.Check())
	{
		return;
	}

	std::vector<uint32_t> expedition_ids;

	// check cache for expired or empty expeditions to delete and notify zones.
	for (auto it = m_expeditions.begin(); it != m_expeditions.end();)
	{
		bool is_deleted = false;

		if (it->IsEmpty() || it->IsExpired())
		{
			// don't delete expedition until its dz instance is empty. this prevents
			// an exploit where all members leave expedition and complete an event
			// before being kicked from removal timer. the lockout could never be
			// applied because the zone expedition cache was already invalidated.
			auto dz_zoneserver = zoneserver_list.FindByInstanceID(it->GetInstanceID());
			if (!dz_zoneserver || dz_zoneserver->NumPlayers() == 0)
			{
				LogExpeditions("Expedition [{}] expired or empty, notifying zones and deleting", it->GetID());
				expedition_ids.emplace_back(it->GetID());
				it->SendZonesExpeditionDeleted();
				is_deleted = true;
			}

			if (it->IsEmpty() && !it->IsPendingDelete() && RuleB(Expedition, EmptyDzShutdownEnabled))
			{
				it->UpdateDzSecondsRemaining(RuleI(Expedition, EmptyDzShutdownDelaySeconds));
			}

			it->SetPendingDelete(true);
		}

		it = is_deleted ? m_expeditions.erase(it) : it + 1;
	}

	if (!expedition_ids.empty())
	{
		ExpeditionDatabase::DeleteExpeditions(expedition_ids);
	}
}

void ExpeditionDatabase::PurgeExpiredExpeditions()
{
	LogExpeditionsDetail("Purging expired expeditions");

	std::string query = SQL(
		DELETE expedition FROM expedition_details expedition
			LEFT JOIN instance_list ON expedition.instance_id = instance_list.id
			LEFT JOIN (
				SELECT expedition_id, COUNT(IF(is_current_member = TRUE, 1, NULL)) member_count
				FROM expedition_members
				GROUP BY expedition_id
			) AS expedition_members
			ON expedition_members.expedition_id = expedition.id
		WHERE
			expedition.instance_id IS NULL
			OR expedition_members.member_count = 0
			OR (instance_list.start_time + instance_list.duration) <= UNIX_TIMESTAMP();
	);

	database.QueryDatabase(query);
}

void ExpeditionDatabase::PurgeExpiredCharacterLockouts()
{
	LogExpeditionsDetail("Purging expired lockouts");

	std::string query = SQL(
		DELETE FROM expedition_character_lockouts
		WHERE expire_time <= NOW();
	);

	database.QueryDatabase(query);
}

std::vector<Expedition> ExpeditionDatabase::LoadExpeditions()
{
	LogExpeditionsDetail("Loading expeditions for world cache");

	std::vector<Expedition> expeditions;

	std::string query = SQL(
		SELECT
			expedition_details.id,
			expedition_details.instance_id,
			instance_list.zone,
			instance_list.start_time,
			instance_list.duration,
			expedition_members.character_id
		FROM expedition_details
			INNER JOIN instance_list ON expedition_details.instance_id = instance_list.id
			INNER JOIN expedition_members
				ON expedition_members.expedition_id = expedition_details.id
				AND expedition_members.is_current_member = TRUE
		ORDER BY expedition_details.id;
	);

	auto results = database.QueryDatabase(query);
	if (results.Success())
	{
		uint32_t last_expedition_id = 0;

		for (auto row = results.begin(); row != results.end(); ++row)
		{
			uint32_t expedition_id = strtoul(row[0], nullptr, 10);

			if (last_expedition_id != expedition_id)
			{
				expeditions.emplace_back(Expedition{
					static_cast<uint32_t>(strtoul(row[0], nullptr, 10)), // expedition_id
					static_cast<uint32_t>(strtoul(row[1], nullptr, 10)), // dz_instance_id
					static_cast<uint32_t>(strtoul(row[2], nullptr, 10)), // dz_zone_id
					static_cast<uint32_t>(strtoul(row[3], nullptr, 10)), // start_time
					static_cast<uint32_t>(strtoul(row[4], nullptr, 10))  // duration
				});
			}

			last_expedition_id = expedition_id;

			uint32_t member_id = static_cast<uint32_t>(strtoul(row[5], nullptr, 10));
			expeditions.back().AddMember(member_id);
		}
	}

	return expeditions;
}

Expedition ExpeditionDatabase::LoadExpedition(uint32_t expedition_id)
{
	LogExpeditions("Loading expedition [{}] for world cache", expedition_id);

	Expedition expedition;

	std::string query = fmt::format(SQL(
		SELECT
			expedition_details.id,
			expedition_details.instance_id,
			instance_list.zone,
			instance_list.start_time,
			instance_list.duration,
			expedition_members.character_id
		FROM expedition_details
			INNER JOIN instance_list ON expedition_details.instance_id = instance_list.id
			INNER JOIN expedition_members
				ON expedition_members.expedition_id = expedition_details.id
				AND expedition_members.is_current_member = TRUE
		WHERE expedition_details.id = {};
	), expedition_id);

	auto results = database.QueryDatabase(query);
	if (results.Success())
	{
		bool created = false;
		for (auto row = results.begin(); row != results.end(); ++row)
		{
			if (!created)
			{
				expedition = Expedition{
					static_cast<uint32_t>(strtoul(row[0], nullptr, 10)), // expedition_id
					static_cast<uint32_t>(strtoul(row[1], nullptr, 10)), // dz_instance_id
					static_cast<uint32_t>(strtoul(row[2], nullptr, 10)), // dz_zone_id
					static_cast<uint32_t>(strtoul(row[3], nullptr, 10)), // start_time
					static_cast<uint32_t>(strtoul(row[4], nullptr, 10))  // duration
				};
				created = true;
			}

			auto member_id = static_cast<uint32_t>(strtoul(row[5], nullptr, 10));
			expedition.AddMember(member_id);
		}
	}

	return expedition;
}

void ExpeditionDatabase::DeleteExpeditions(const std::vector<uint32_t>& expedition_ids)
{
	std::string expedition_ids_query;
	for (const auto& expedition_id : expedition_ids)
	{
		fmt::format_to(std::back_inserter(expedition_ids_query), "{},", expedition_id);
	}

	if (!expedition_ids_query.empty())
	{
		expedition_ids_query.pop_back(); // trailing comma

		std::string query = fmt::format(
			"DELETE FROM expedition_details WHERE id IN ({});", expedition_ids_query
		);
		database.QueryDatabase(query);

		// todo: if not using foreign key constraints
		//query = fmt::format(
		//	"DELETE FROM expedition_members WHERE expedition_id IN ({});", expedition_ids_query
		//);
		//database.QueryDatabase(query);

		//query = fmt::format(
		//	"DELETE FROM expedition_lockouts WHERE expedition_id IN ({});", expedition_ids_query
		//);
		//database.QueryDatabase(query);
	}
}

void ExpeditionDatabase::UpdateDzDuration(uint16_t instance_id, uint32_t new_duration)
{
	std::string query = fmt::format(
		"UPDATE instance_list SET duration = {} WHERE id = {};",
		new_duration, instance_id
	);

	database.QueryDatabase(query);
}

void ExpeditionMessage::HandleZoneMessage(ServerPacket* pack)
{
	switch (pack->opcode)
	{
	case ServerOP_ExpeditionCreate:
	{
		auto buf = reinterpret_cast<ServerExpeditionID_Struct*>(pack->pBuffer);
		expedition_cache.AddExpedition(buf->expedition_id);
		zoneserver_list.SendPacket(pack);
		break;
	}
	case ServerOP_ExpeditionMemberChange:
	{
		auto buf = reinterpret_cast<ServerExpeditionMemberChange_Struct*>(pack->pBuffer);
		expedition_cache.MemberChange(buf->expedition_id, buf->char_id, buf->removed);
		zoneserver_list.SendPacket(pack);
		break;
	}
	case ServerOP_ExpeditionMemberSwap:
	{
		auto buf = reinterpret_cast<ServerExpeditionMemberSwap_Struct*>(pack->pBuffer);
		expedition_cache.MemberChange(buf->expedition_id, buf->remove_char_id, true);
		expedition_cache.MemberChange(buf->expedition_id, buf->add_char_id, false);
		zoneserver_list.SendPacket(pack);
		break;
	}
	case ServerOP_ExpeditionMembersRemoved:
	{
		auto buf = reinterpret_cast<ServerExpeditionID_Struct*>(pack->pBuffer);
		expedition_cache.RemoveAllMembers(buf->expedition_id);
		zoneserver_list.SendPacket(pack);
		break;
	}
	case ServerOP_ExpeditionGetOnlineMembers:
	{
		ExpeditionMessage::GetOnlineMembers(pack);
		break;
	}
	case ServerOP_ExpeditionDzAddPlayer:
	{
		ExpeditionMessage::AddPlayer(pack);
		break;
	}
	case ServerOP_ExpeditionDzMakeLeader:
	{
		ExpeditionMessage::MakeLeader(pack);
		break;
	}
	case ServerOP_ExpeditionRemoveCharLockouts:
	{
		auto buf = reinterpret_cast<ServerExpeditionCharacterLockout_Struct*>(pack->pBuffer);
		client_list.SendPacket(buf->character_name, pack);
		break;
	}
	case ServerOP_ExpeditionSaveInvite:
	{
		ExpeditionMessage::SaveInvite(pack);
		break;
	}
	case ServerOP_ExpeditionRequestInvite:
	{
		ExpeditionMessage::RequestInvite(pack);
		break;
	}
	}
}

void ExpeditionMessage::AddPlayer(ServerPacket* pack)
{
	auto buf = reinterpret_cast<ServerDzCommand_Struct*>(pack->pBuffer);

	ClientListEntry* invited_cle = client_list.FindCharacter(buf->target_name);
	if (invited_cle && invited_cle->Server())
	{
		// continue in the add target's zone
		buf->is_char_online = true;
		invited_cle->Server()->SendPacket(pack);
	}
	else
	{
		// add target not online, return to inviter
		ClientListEntry* inviter_cle = client_list.FindCharacter(buf->requester_name);
		if (inviter_cle && inviter_cle->Server())
		{
			inviter_cle->Server()->SendPacket(pack);
		}
	}
}

void ExpeditionMessage::MakeLeader(ServerPacket* pack)
{
	auto buf = reinterpret_cast<ServerDzCommand_Struct*>(pack->pBuffer);

	// notify requester (old leader) and new leader of the result
	ZoneServer* new_leader_zs = nullptr;
	ClientListEntry* new_leader_cle = client_list.FindCharacter(buf->target_name);
	if (new_leader_cle && new_leader_cle->Server())
	{
		buf->is_char_online = true;
		new_leader_zs = new_leader_cle->Server();
		new_leader_zs->SendPacket(pack);
	}

	// if old and new leader are in the same zone only send one message
	ClientListEntry* requester_cle = client_list.FindCharacter(buf->requester_name);
	if (requester_cle && requester_cle->Server() && requester_cle->Server() != new_leader_zs)
	{
		requester_cle->Server()->SendPacket(pack);
	}
}

void ExpeditionMessage::GetOnlineMembers(ServerPacket* pack)
{
	auto buf = reinterpret_cast<ServerExpeditionCharacters_Struct*>(pack->pBuffer);

	// not efficient but only requested during caching
	char zone_name[64] = {0};
	std::vector<ClientListEntry*> all_clients;
	all_clients.reserve(client_list.GetClientCount());
	client_list.GetClients(zone_name, all_clients);

	for (uint32_t i = 0; i < buf->count; ++i)
	{
		auto it = std::find_if(all_clients.begin(), all_clients.end(), [&](const ClientListEntry* cle) {
			return (cle && cle->CharID() == buf->entries[i].character_id);
		});

		if (it != all_clients.end())
		{
			buf->entries[i].character_zone_id = (*it)->zone();
			buf->entries[i].character_instance_id = (*it)->instance();
			buf->entries[i].character_online = true;
		}
	}

	zoneserver_list.SendPacket(buf->sender_zone_id, buf->sender_instance_id, pack);
}

void ExpeditionMessage::SaveInvite(ServerPacket* pack)
{
	auto buf = reinterpret_cast<ServerDzCommand_Struct*>(pack->pBuffer);

	ClientListEntry* invited_cle = client_list.FindCharacter(buf->target_name);
	if (invited_cle)
	{
		// store packet on cle and re-send it when client requests it
		buf->is_char_online = true;
		pack->opcode = ServerOP_ExpeditionDzAddPlayer;
		invited_cle->SetPendingExpeditionInvite(pack);
	}
}

void ExpeditionMessage::RequestInvite(ServerPacket* pack)
{
	auto buf = reinterpret_cast<ServerExpeditionCharacterID_Struct*>(pack->pBuffer);
	ClientListEntry* cle = client_list.FindCLEByCharacterID(buf->character_id);
	if (cle)
	{
		auto invite_pack = cle->GetPendingExpeditionInvite();
		if (invite_pack && cle->Server())
		{
			cle->Server()->SendPacket(invite_pack.get());
		}
	}
}
