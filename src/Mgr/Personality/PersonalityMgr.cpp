/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option), any later version.
 */

#include "PersonalityMgr.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "PlayerbotAIConfig.h"
#include "QueryResult.h"

void PersonalityMgr::Load()
{
    std::lock_guard<std::mutex> lock(m_lock);
    m_personalities.clear();
    // keep bot mapping unless we explicitly reload
    if (!m_loaded)
        m_botPersonality.clear();

    // Load personalities from DB. If empty, fall back to hardcoded defaults.
    QueryResult result = PlayerbotsDatabase.Query("SELECT id, name, suffix FROM playerbots_personality");
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            PersonalityEntry e;
            e.id = fields[0].Get<uint8>();
            e.name = fields[1].Get<std::string>();
            e.suffix = fields[2].Get<std::string>();
            m_personalities[e.id] = std::move(e);
        } while (result->NextRow());
        LOG_INFO("playerbots", "Loaded {} bot personalities", m_personalities.size());
    }

    if (m_personalities.empty())
    {
        LOG_WARN("playerbots", "No personalities in DB, using hardcoded fallback");
        m_personalities[1] = {1, "stalwart_defender", "You are a gruff Ironforge veteran. Short, stoic sentences. You value honor, ale and oaths."};
        m_personalities[2] = {2, "serene_keeper", "You are a serene Night Elf keeper. Calm, nature-bound, you speak with forest metaphors."};
        m_personalities[3] = {3, "cynical_survivor", "You are a cynical Forsaken survivor. Dark humor, terse, dry wit."};
        m_personalities[4] = {4, "haughty_magister", "You are a haughty Blood Elf magister. Elegant, arcane, slightly arrogant."};
        m_personalities[5] = {5, "savage_warmonger", "You are a savage Orc warmonger. Guttural, honor-driven, fierce."};
        m_personalities[6] = {6, "mischievous_tinkerer", "You are a mischievous Gnome tinkerer. Playful, clever, riddle-like."};
        m_personalities[7] = {7, "stoic_shield", "You are a stoic Tauren shield. Earthy, calm, you speak of the Earth Mother."};
        m_personalities[8] = {8, "shadow_whisper", "You are a shadowy Troll whisperer. Jungle slang, cunning, lyrical."};
        m_personalities[9] = {9, "disciplined_vanguard", "You are a disciplined Human vanguard. Formal, dutiful, courage."};
        m_personalities[10] = {10, "wild_hunter", "You are a wild hunter. Feral, tracking-focused, keen instinct."};
        m_personalities[11] = {11, "arcane_seeker", "You are an arcane seeker, precise and studious. You love tomes and runes."};
        m_personalities[12] = {12, "lightsworn", "You are a Light-sworn Draenei anchorite. Serene, luminous, hopeful."};
    }

    // Load bot->personality mapping
    QueryResult botRes = PlayerbotsDatabase.Query("SELECT bot_guid, personality_id FROM playerbots_bot_personality");
    if (botRes)
    {
        do
        {
            Field* fields = botRes->Fetch();
            uint32 botGuid = fields[0].Get<uint32>();
            uint8 pid = fields[1].Get<uint8>();
            if (m_personalities.find(pid) != m_personalities.end())
                m_botPersonality[botGuid] = pid;
        } while (botRes->NextRow());
        LOG_INFO("playerbots", "Loaded {} bot-personality assignments", m_botPersonality.size());
    }

    m_loaded = true;
}

bool PersonalityMgr::HasPersonality(uint32 botGuid) const
{
    // const method - need to cast lock
    // caller should hold lock or accept race; for simplicity do not lock here
    return m_botPersonality.find(botGuid) != m_botPersonality.end();
}

bool PersonalityMgr::IsRaceClassFit(uint8 personalityId, uint8 race, uint8 cls) const
{
    // Simple race/class fit matrix. Returns true if personality suits race+class.
    // This is intentionally permissive; fallback is any personality.
    switch (personalityId)
    {
        case 1: // stalwart
            return (race == 3 || race == 1) && (cls == 1 || cls == 2 || cls == 4);
        case 2: // serene
            return (race == 4 || race == 11) && (cls == 11 || cls == 5 || cls == 3);
        case 3: // cynical
            return (race == 5 || race == 8) && (cls == 4 || cls == 9 || cls == 5);
        case 4: // haughty
            return (race == 10 || race == 7) && (cls == 8 || cls == 2 || cls == 9);
        case 5: // savage
            return (race == 2 || race == 6 || race == 8) && (cls == 1 || cls == 7 || cls == 3);
        case 6: // mischievous
            return (race == 7 || race == 3) && (cls == 8 || cls == 4 || cls == 9);
        case 7: // stoic
            return (race == 6 || race == 2) && (cls == 11 || cls == 1 || cls == 7);
        case 8: // shadow
            return (race == 8 || race == 5 || race == 2) && (cls == 3 || cls == 5 || cls == 4);
        case 9: // disciplined
            return (race == 1 || race == 3 || race == 11) && (cls == 2 || cls == 1 || cls == 6);
        case 10: // wild
            return (cls == 3 || cls == 11 || cls == 7);
        case 11: // arcane
            return (cls == 8 || cls == 9 || cls == 5);
        case 12: // lightsworn
            return (race == 11 || race == 10 || race == 1) && (cls == 5 || cls == 2);
        default:
            return true;
    }
}

uint8 PersonalityMgr::PickDeterministic(uint32 botGuid, uint8 race, uint8 cls)
{
    if (m_personalities.empty())
        return 1;

    // Gather candidates that fit race/class
    std::vector<uint8> candidates;
    for (auto const& kv : m_personalities)
    {
        if (IsRaceClassFit(kv.first, race, cls))
            candidates.push_back(kv.first);
    }
    if (candidates.empty())
    {
        for (auto const& kv : m_personalities)
            candidates.push_back(kv.first);
    }

    // Deterministic hash: botGuid + race*17 + class*31
    uint32 h = botGuid * 2654435761u ^ (uint32(race) * 16777619u) ^ (uint32(cls) * 2166136261u);
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    return candidates[h % candidates.size()];
}

uint8 PersonalityMgr::GetPersonalityId(uint32 botGuid, uint8 race, uint8 cls)
{
    {
        std::lock_guard<std::mutex> lock(m_lock);
        auto it = m_botPersonality.find(botGuid);
        if (it != m_botPersonality.end())
            return it->second;
    }

    uint8 pid = 0;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        auto it = m_botPersonality.find(botGuid);
        if (it != m_botPersonality.end())
            return it->second;

        pid = PickDeterministic(botGuid, race, cls);
        m_botPersonality[botGuid] = pid;
    }

    // Persist outside lock so DB queue does not block
    PlayerbotsDatabase.Execute("INSERT IGNORE INTO playerbots_bot_personality (bot_guid, personality_id) VALUES ({}, {})", botGuid, uint32(pid));
    LOG_DEBUG("playerbots", "Assigned personality {} to bot {}", uint32(pid), botGuid);
    return pid;
}

std::string PersonalityMgr::GetPersonalitySuffix(uint32 botGuid, uint8 race, uint8 cls)
{
    if (!sPlayerbotAIConfig.llmPersonalitiesEnabled)
        return "";

    uint8 pid = GetPersonalityId(botGuid, race, cls);
    std::lock_guard<std::mutex> lock(m_lock);
    auto it = m_personalities.find(pid);
    if (it != m_personalities.end())
        return it->second.suffix;
    return "";
}
