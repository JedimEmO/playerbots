#include "playerbot/playerbot.h"

#include "playerbot/GuildTactics.h"
#include "playerbot/GuildBotMgr.h"
#include "playerbot/ServerFacade.h"
#include "AI/EventAI/CreatureEventAI.h"
#include "AI/EventAI/CreatureEventAIMgr.h"
#include "Entities/Creature.h"
#include "Groups/Group.h"
#include "Spells/Spell.h"
#include "Spells/SpellMgr.h"

#include <algorithm>
#include <sstream>

// The pure rules classify effects by the core's numbers; each must be the core's own.
static_assert(runtactics::effect::APPLY_AURA == SPELL_EFFECT_APPLY_AURA, "effect number");
static_assert(runtactics::effect::HEAL == SPELL_EFFECT_HEAL, "effect number");
static_assert(runtactics::effect::SUMMON == SPELL_EFFECT_SUMMON, "effect number");
static_assert(runtactics::effect::TRIGGER_MISSILE == SPELL_EFFECT_TRIGGER_MISSILE, "effect number");
static_assert(runtactics::effect::SUMMON_WILD == SPELL_EFFECT_SUMMON_WILD, "effect number");
static_assert(runtactics::effect::SUMMON_GUARDIAN == SPELL_EFFECT_SUMMON_GUARDIAN, "effect number");
static_assert(runtactics::effect::TRIGGER_SPELL == SPELL_EFFECT_TRIGGER_SPELL, "effect number");
static_assert(runtactics::effect::HEAL_MAX_HEALTH == SPELL_EFFECT_HEAL_MAX_HEALTH, "effect number");
static_assert(runtactics::effect::INTERRUPT_CAST == SPELL_EFFECT_INTERRUPT_CAST, "effect number");
static_assert(runtactics::effect::SUMMON_POSSESSED == SPELL_EFFECT_SUMMON_POSSESSED, "effect number");
static_assert(runtactics::effect::SUMMON_TOTEM == SPELL_EFFECT_SUMMON_TOTEM, "effect number");
static_assert(runtactics::effect::HEAL_MECHANICAL == SPELL_EFFECT_HEAL_MECHANICAL, "effect number");
static_assert(runtactics::effect::SUMMON_TOTEM_SLOT1 == SPELL_EFFECT_SUMMON_TOTEM_SLOT1, "effect number");
static_assert(runtactics::effect::SUMMON_TOTEM_SLOT4 == SPELL_EFFECT_SUMMON_TOTEM_SLOT4, "effect number");
static_assert(runtactics::effect::SUMMON_PHANTASM == SPELL_EFFECT_SUMMON_PHANTASM, "effect number");
static_assert(runtactics::effect::SUMMON_DEMON == SPELL_EFFECT_SUMMON_DEMON, "effect number");
static_assert(runtactics::effect::AURA_PERIODIC_HEAL == SPELL_AURA_PERIODIC_HEAL, "aura number");

std::mutex GuildTactics::cacheLock;
std::map<uint32, uint8> GuildTactics::spellKinds;

namespace
{
    // The raid icons the run sets (RtiTargetValue's indexes).
    constexpr uint8 ICON_SKULL = 7;
    constexpr uint8 ICON_CROSS = 6;
    // How long after its pull the stop's pack counts as foes, coming, though not yet in the fight.
    constexpr time_t PULL_COMING_SECONDS = 10;
    // A cast that ends this soon before its time is up counts as done, not stopped.
    constexpr uint32 CAST_SLACK_MS = 300;

    bool Settled(Player* bot)
    {
        return bot && bot->IsInWorld() && !bot->IsBeingTeleported();
    }

    runtactics::Type TypeOf(uint32 creatureType)
    {
        switch (creatureType)
        {
            case CREATURE_TYPE_HUMANOID: return runtactics::Type::Humanoid;
            case CREATURE_TYPE_BEAST: return runtactics::Type::Beast;
            case CREATURE_TYPE_UNDEAD: return runtactics::Type::Undead;
            case CREATURE_TYPE_ELEMENTAL: return runtactics::Type::Elemental;
            case CREATURE_TYPE_MECHANICAL: return runtactics::Type::Mechanical;
            case CREATURE_TYPE_DEMON: return runtactics::Type::Demon;
            case CREATURE_TYPE_DRAGONKIN: return runtactics::Type::Dragonkin;
            case CREATURE_TYPE_GIANT: return runtactics::Type::Giant;
            default: return runtactics::Type::Other;
        }
    }

    uint8 KindOf(uint32 spellId, int depth)
    {
        const SpellEntry* entry = sServerFacade.LookupSpellInfo(spellId);
        if (!entry)
            return runtactics::KIND_NONE;
        uint8 kind = runtactics::KIND_NONE;
        for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            kind |= runtactics::EffectKind(entry->Effect[i], entry->EffectApplyAuraName[i]);
            if (depth == 0 && runtactics::Triggers(entry->Effect[i]) && entry->EffectTriggerSpell[i] && entry->EffectTriggerSpell[i] != spellId)
                kind |= KindOf(entry->EffectTriggerSpell[i], depth + 1);
        }
        return kind;
    }

    std::string KindName(uint8 kind)
    {
        if (kind == (runtactics::KIND_SUMMON | runtactics::KIND_HEAL))
            return "summon+heal";
        return kind & runtactics::KIND_SUMMON ? "summon" : kind & runtactics::KIND_HEAL ? "heal" : "none";
    }

    // The party's members in the leader's map: the run's expected ones present there.
    std::vector<Player*> PartyHere(Player* leader, const GuildRunView& view)
    {
        std::vector<Player*> party;
        Map* map = leader->GetMap();
        for (uint32 guid : view.expected)
        {
            Player* member = map->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid));
            if (member && member->IsInWorld() && !member->IsBeingTeleported())
                party.push_back(member);
        }
        return party;
    }

    std::vector<Player*> Healers(const std::vector<Player*>& party)
    {
        std::vector<Player*> healers;
        for (Player* member : party)
            if (member->IsAlive() && PlayerbotAI::IsHeal(member))
                healers.push_back(member);
        return healers;
    }

    runtactics::Role RoleOf(Player* member)
    {
        if (PlayerbotAI::IsTank(member))
            return runtactics::Role::Tank;
        return PlayerbotAI::IsHeal(member) ? runtactics::Role::Healer : runtactics::Role::Dps;
    }

    // Whether the engine plays this member for the run: an AI of its own, and not a person's (view.persons).
    bool EnginePlays(Player* member, const GuildRunView& view)
    {
        PlayerbotAI* ai = member->GetPlayerbotAI();
        return ai && !ai->IsRealPlayer() && std::find(view.persons.begin(), view.persons.end(), member->GetGUIDLow()) == view.persons.end();
    }

    // The first of the member's interrupts its AI can cast at the unit now: known, with the resources, off cooldown, in
    // range and in sight (PlayerbotAI::CanCastSpell). 0 for none.
    uint32 Castable(Player* member, const std::vector<uint32>& interrupts, Unit* unit)
    {
        PlayerbotAI* ai = member->GetPlayerbotAI();
        if (!ai || !unit)
            return 0;
        for (uint32 spell : interrupts)
            if (ai->CanCastSpell(spell, unit, 0, true))
                return spell;
        return 0;
    }

    // Whom the creature attacks, from the tank's side: the bot itself or another tank of its party, a living healer of
    // its party (FoeOf's onHealer), another living member of its party, or anything else, a pet included. Once the tank
    // has it, it is on the tank. A creature under crowd control that damage breaks (a sheep, a sap, a freezing trap),
    // whoever cast it, is on nobody: it keeps its victim while it cannot act (Unit::SelectHostileTarget returns early),
    // and the tank's blow would break it.
    runtactics::Victim VictimOf(Unit* creature, Player* bot)
    {
        if (creature->HasBreakableByDamageCrowdControlAura())
            return runtactics::Victim::None;
        Unit* victim = creature->GetVictim();
        if (victim == bot)
            return runtactics::Victim::Tank;
        if (!victim || victim->GetTypeId() != TYPEID_PLAYER)
            return runtactics::Victim::None;
        Player* player = static_cast<Player*>(victim);
        if (!player->IsAlive() || !bot->IsInGroup(player))
            return runtactics::Victim::None;
        if (PlayerbotAI::IsHeal(player))
            return runtactics::Victim::Healer;
        return PlayerbotAI::IsTank(player) ? runtactics::Victim::Tank : runtactics::Victim::Member;
    }

    const char* VictimName(runtactics::Victim on)
    {
        switch (on)
        {
            case runtactics::Victim::Healer: return "healer";
            case runtactics::Victim::Member: return "member";
            case runtactics::Victim::Tank: return "tank";
            default: return "none";
        }
    }
}

uint8 GuildTactics::SpellKind(uint32 spellId)
{
    if (!spellId)
        return runtactics::KIND_NONE;
    {
        std::lock_guard<std::mutex> guard(cacheLock);
        auto it = spellKinds.find(spellId);
        if (it != spellKinds.end())
            return it->second;
    }
    const uint8 kind = KindOf(spellId, 0);
    std::lock_guard<std::mutex> guard(cacheLock);
    spellKinds[spellId] = kind;
    return kind;
}

GuildTactics::Abilities GuildTactics::AbilitiesOf(Creature* creature)
{
    Abilities a;
    auto note = [&a](uint8 kind, const char* from)
    {
        if (kind & runtactics::KIND_SUMMON)
            a.summons = true;
        if (kind & runtactics::KIND_HEAL)
            a.heals = true;
        if (kind && a.from.find(from) == std::string::npos)
            a.from += (a.from.empty() ? "" : ",") + std::string(from);
    };
    for (auto const& [position, spell] : creature->GetSpellList().Spells)
        note(SpellKind(spell.SpellId), "spell_list");
    // The creature's EventAI, by its entry and by its own spawn: the spells it casts, what it summons, and a flee at low
    // health. Read through a copy of the shared map, which a reload replaces whole.
    auto scan = [&](const std::shared_ptr<CreatureEventAI_Event_Map>& events, uint32 key)
    {
        if (!events || !key)
            return;
        auto it = events->find(key);
        if (it == events->end())
            return;
        for (const CreatureEventAI_Event& event : it->second)
            for (const CreatureEventAI_Action& action : event.action)
            {
                switch (action.type)
                {
                    case ACTION_T_CAST:
                        note(SpellKind(action.cast.spellId), "event_ai");
                        break;
                    case ACTION_T_SPAWN:
                    case ACTION_T_SUMMON_ID:
                        note(runtactics::KIND_SUMMON, "event_ai");
                        break;
                    case ACTION_T_FLEE_FOR_ASSIST:
                        a.flees = true;
                        if (a.from.find("flee") == std::string::npos)
                            a.from += (a.from.empty() ? "" : ",") + std::string("event_ai_flee");
                        break;
                    default:
                        break;
                }
            }
    };
    const std::shared_ptr<CreatureEventAI_Event_Map> byEntry = sEventAIMgr.GetCreatureEventEntryAIMap();
    const std::shared_ptr<CreatureEventAI_Event_Map> byGuid = sEventAIMgr.GetCreatureEventGuidAIMap();
    scan(byEntry, creature->GetEntry());
    scan(byGuid, creature->GetDbGuid());
    return a;
}

std::vector<uint32> GuildTactics::InterruptsOf(Player* player)
{
    // The highest rank known of each interrupt, by its chain.
    std::map<uint32, uint32> best;
    for (auto const& [id, known] : player->GetSpellMap())
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled)
            continue;
        const SpellEntry* entry = sServerFacade.LookupSpellInfo(id);
        if (!entry || IsPassiveSpell(entry))
            continue;
        bool interrupts = false;
        for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
            interrupts |= entry->Effect[i] == SPELL_EFFECT_INTERRUPT_CAST;
        if (!interrupts)
            continue;
        uint32& kept = best[sSpellMgr.GetFirstSpellInChain(id)];
        if (!kept || sSpellMgr.GetSpellRank(id) > sSpellMgr.GetSpellRank(kept))
            kept = id;
    }
    std::vector<uint32> spells;
    for (auto const& [first, id] : best)
        spells.push_back(id);
    return spells;
}

Spell* GuildTactics::CastOf(Unit* unit, uint8& kind)
{
    kind = runtactics::KIND_NONE;
    for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
        if (Spell* spell = unit->GetCurrentSpell(type))
            if (spell->m_spellInfo)
            {
                kind = SpellKind(spell->m_spellInfo->Id);
                if (kind)
                    return spell;
            }
    kind = runtactics::KIND_NONE;
    return nullptr;
}

std::vector<Creature*> GuildTactics::FightCreatures(Player* leader, const GuildRunView& view, Creature* pulled)
{
    std::vector<Creature*> creatures;
    Map* map = leader->GetMap();
    auto add = [&](Unit* unit)
    {
        if (!unit || unit->GetTypeId() != TYPEID_UNIT || !unit->IsInWorld() || !unit->IsAlive() || unit->GetMap() != map)
            return;
        Creature* creature = static_cast<Creature*>(unit);
        if (creature->IsTotem() || std::find(creatures.begin(), creatures.end(), creature) != creatures.end())
            return;
        creatures.push_back(creature);
    };
    for (Player* member : PartyHere(leader, view))
    {
        if (!member->IsAlive())
            continue;
        for (Unit* attacker : member->getAttackers())
            add(attacker);
        // What a member attacks, once in a fight: not a creature it goes at before its first blow lands, nor one that left
        // the fight (evaded after a pull that brought nothing), which marked would be struck where it stands.
        if (Unit* victim = member->GetVictim())
            if (victim->IsInCombat())
                add(victim);
    }
    // At the pull, the creature pulled; and while the pull comes, it and those of the stop's pack, once in a fight, before
    // they reach a member. One not yet in the fight is never a foe: marked, the party would go and strike it where it stands.
    const bool coming = view.pull == GuildRunPull::Pull && time(nullptr) - view.pullAt < PULL_COMING_SECONDS;
    if (pulled && pulled->IsInCombat())
        add(pulled);
    if (coming && !view.pulled.IsEmpty())
        if (Creature* creature = map->GetAnyTypeCreature(view.pulled))
            if (creature->IsInCombat())
                add(creature);
    if (pulled || coming)
        for (uint32 spawn : view.stop.spawns)
            if (Creature* creature = map->GetCreature(spawn))
                if (creature->IsInCombat() && sServerFacade.IsHostileTo(creature, leader))
                    add(creature);
    return creatures;
}

GuildTactics::Live& GuildTactics::LiveOf(Player* leader, const GuildRunView& view)
{
    Live& live = lives[view.instanceId];
    // Another generation, or another leader: what the plan saw goes; the marks it set are kept, so that the next
    // decision, or the sweep, clears them where they still stand.
    if (live.leader != leader->GetGUIDLow() || (live.stamp.generation && live.stamp.generation != view.stamp.generation))
    {
        const ObjectGuid skull = live.skull, cross = live.cross;
        live = Live();
        live.skull = skull;
        live.cross = cross;
    }
    live.leader = leader->GetGUIDLow();
    live.group = leader->GetGroup() ? leader->GetGroup()->GetId() : 0;
    live.mapId = leader->GetMapId();
    live.instanceId = view.instanceId;
    live.start = view.start;
    return live;
}

std::string GuildTactics::Tag(const Live& live)
{
    return "gen=" + std::to_string(live.start) + "." + std::to_string(live.stamp.generation) + " rev=" + std::to_string(live.stamp.plan);
}

std::string GuildTactics::FoeText(const Live& live, uint32 id)
{
    auto it = live.guids.find(id);
    if (!id || it == live.guids.end())
        return "none";
    return std::to_string(it->second.GetEntry()) + "." + std::to_string(it->second.GetCounter());
}

std::string GuildTactics::CastEnded(const Live& live, const Known& known, const char* how, uint32 nowMs)
{
    return Tag(live) + " cast=" + FoeText(live, known.id) + ":" + std::to_string(known.castSpell) + " kind=" + KindName(known.castKind)
        + " end=" + how + " ms=" + std::to_string(WorldTimer::getMSTimeDiff(known.castStartMs, nowMs)) + " of=" + std::to_string(known.castMs)
        + " interruptible=" + (known.castInterruptible ? "1" : "0") + " tried=" + std::to_string(known.tried)
        + " taker=" + std::to_string(runtactics::InterrupterOf(live.plan, known.id));
}

runtactics::Foe GuildTactics::FoeOf(Live& live, Creature* creature, const std::vector<Player*>& healers, uint32 nowMs,
    std::vector<std::string>& events)
{
    Known& known = live.known[creature->GetObjectGuid()];
    if (!known.id)
    {
        known.id = ++live.nextId;
        live.guids[known.id] = creature->GetObjectGuid();
    }
    if (known.abilities.from.empty() && !known.abilities.summons && !known.abilities.heals && !known.abilities.flees)
        known.abilities = AbilitiesOf(creature);
    // Seen: a summon in the fight makes its summoner one; a flee, a flee-er.
    const ObjectGuid summoner = creature->IsTemporarySummon() ? creature->GetSpawnerGuid() : creature->IsPet() ? creature->GetOwnerGuid() : ObjectGuid();
    if (summoner.IsCreatureOrPet())
    {
        Known& by = live.known[summoner];
        if (!by.id)
        {
            by.id = ++live.nextId;
            live.guids[by.id] = summoner;
        }
        by.seenSummoning = true;
    }
    if (creature->hasUnitState(UNIT_STAT_FLEEING | UNIT_STAT_RETREATING))
        known.seenFleeing = true;

    // Its cast: a new one is followed until it ends, which Look tells.
    uint8 kind = runtactics::KIND_NONE;
    Spell* cast = CastOf(creature, kind);
    const uint32 spell = cast ? cast->m_spellInfo->Id : 0;
    if (known.castSpell && known.castSpell != spell)
    {
        const bool done = WorldTimer::getMSTimeDiff(known.castStartMs, nowMs) + CAST_SLACK_MS >= known.castMs;
        events.push_back(CastEnded(live, known, known.interrupted ? "interrupted" : done ? "done" : "stopped", nowMs));
        runtactics::Ended(live.plan, known.id);
        known.castSpell = 0;
    }
    if (spell && !known.castSpell)
    {
        known.castSpell = spell;
        known.castKind = kind;
        known.castStartMs = nowMs;
        known.castMs = uint32(std::max(0, cast->GetCastTime()));
        known.castInterruptible = cast->IsInterruptible();
        known.tried = 0;
        known.interrupted = false;
    }
    if (kind & runtactics::KIND_SUMMON)
        known.seenSummoning = true;
    if (kind & runtactics::KIND_HEAL)
        known.seenHealing = true;

    runtactics::Foe foe;
    foe.guid = known.id;
    const CreatureInfo* info = creature->GetCreatureInfo();
    foe.type = info ? TypeOf(info->CreatureType) : runtactics::Type::Other;
    foe.elite = creature->IsElite();
    foe.health = uint32(creature->GetHealthPercent());
    foe.summons = known.abilities.summons || known.seenSummoning;
    foe.heals = known.abilities.heals || known.seenHealing;
    foe.flees = known.abilities.flees || known.seenFleeing;
    foe.fleeing = creature->hasUnitState(UNIT_STAT_FLEEING | UNIT_STAT_RETREATING);
    foe.casting = spell != 0;
    foe.interruptible = cast && cast->IsInterruptible();
    Unit* victim = creature->GetVictim();
    foe.onHealer = victim && std::find(healers.begin(), healers.end(), victim) != healers.end();
    foe.inFight = creature->IsInCombat();
    return foe;
}

std::vector<runtactics::Member> GuildTactics::Members(Player* leader, const GuildRunView& view,
    const std::vector<std::pair<uint32, Unit*>>& casting, std::map<uint32, std::string>& names)
{
    std::vector<std::pair<int, runtactics::Member>> ranked;
    for (Player* member : PartyHere(leader, view))
    {
        runtactics::Member m;
        m.guid = member->GetGUIDLow();
        m.alive = member->IsAlive();
        m.engine = EnginePlays(member, view);
        names[m.guid] = member->GetName();
        // The first experiment: interrupts only, no crowd control and no snares. A member is asked about each caster only
        // when there is one, through its own AI's check of the spell.
        if (m.engine && m.alive && !casting.empty())
        {
            const std::vector<uint32> interrupts = InterruptsOf(member);
            for (auto const& [id, unit] : casting)
            {
                if (Castable(member, interrupts, unit))
                    m.interrupt = true;
                else
                    m.beyond.push_back(id);
            }
        }
        ranked.push_back({ runtactics::RoleRank(RoleOf(member)), m });
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    std::vector<runtactics::Member> members;
    for (auto& [rank, m] : ranked)
        members.push_back(m);
    return members;
}

bool GuildTactics::Look(Player* leader, const GuildRunView& view, std::string& why)
{
    if (!Settled(leader) || !leader->IsAlive() || !view.valid || view.mode != GuildRunMode::Lead || !view.lead)
        return false;
    const std::vector<Player*> party = PartyHere(leader, view);
    const bool fighting = std::any_of(party.begin(), party.end(), [](Player* m) { return m->IsAlive() && m->IsInCombat(); });
    const bool coming = view.pull == GuildRunPull::Pull && time(nullptr) - view.pullAt < PULL_COMING_SECONDS;
    const std::vector<Creature*> creatures = FightCreatures(leader, view, nullptr);
    if (!fighting && !coming)
    {
        // Out of a fight with no pull coming. A mark the plan set and still has, on a creature not in a fight now (a pull
        // that started nothing, failed or brought nothing, or a fight over), would send the party at it where it stands
        // as soon as the next fight starts: decide again on the creatures in a fight, none, which clears it.
        if (!leader->GetGroup())
            return false;
        std::set<ObjectGuid> fight;
        for (Creature* creature : creatures)
            fight.insert(creature->GetObjectGuid());
        std::lock_guard<std::mutex> guard(lock);
        auto it = lives.find(view.instanceId);
        if (it == lives.end() || it->second.leader != leader->GetGUIDLow())
            return false;
        const Live& live = it->second;
        const bool stray = (!live.skull.IsEmpty() && !fight.count(live.skull)) || (!live.cross.IsEmpty() && !fight.count(live.cross));
        if (stray)
            why = "clear";
        return stray;
    }
    const std::vector<Player*> healers = Healers(party);
    const uint32 nowMs = WorldTimer::getMSTime();
    std::vector<std::string> events;
    bool due = false;
    uint32 leaderGuid = leader->GetGUIDLow();
    {
        std::lock_guard<std::mutex> guard(lock);
        Live& live = LiveOf(leader, view);
        std::vector<runtactics::Foe> foes;
        std::set<ObjectGuid> here;
        for (Creature* creature : creatures)
        {
            foes.push_back(FoeOf(live, creature, healers, nowMs, events));
            here.insert(creature->GetObjectGuid());
        }
        // A cast followed whose caster is no longer in the fight: dead, or gone from it.
        Map* map = leader->GetMap();
        for (auto& [guid, known] : live.known)
        {
            if (!known.castSpell || here.count(guid))
                continue;
            Creature* creature = map->GetAnyTypeCreature(guid);
            events.push_back(CastEnded(live, known, known.interrupted ? "interrupted" : !creature || !creature->IsAlive() ? "died" : "gone", nowMs));
            runtactics::Ended(live.plan, known.id);
            known.castSpell = 0;
        }
        if (!live.stamp.generation || !runrules::Same(live.stamp, view.stamp))
        {
            // No plan under this stamp: one is due when there is anything to plan for, or marks of its own to clear.
            due = !foes.empty() || !live.skull.IsEmpty() || !live.cross.IsEmpty();
            why = live.stamp.generation ? "stamp" : "fight";
        }
        else if (runtactics::Stale(live.plan, foes, live.planned, false))
        {
            due = true;
            why = "stale";
        }
        else if (runtactics::Due(live.plan, foes, live.planned, int64_t(nowMs)))
        {
            due = true;
            why = "retry";
        }
    }
    for (const std::string& detail : events)
        sGuildBotMgr.RecordEvent(leaderGuid, "run_tactic", detail);
    return due;
}

bool GuildTactics::Decide(Player* leader, const GuildRunView& view, const std::string& why, Creature* pulled)
{
    if (!Settled(leader) || !view.valid || view.mode != GuildRunMode::Lead || !view.lead)
        return false;
    Group* group = leader->GetGroup();
    if (!group)
        return false;
    const std::vector<Player*> party = PartyHere(leader, view);
    const std::vector<Player*> healers = Healers(party);
    const std::vector<Creature*> creatures = FightCreatures(leader, view, pulled);
    const uint32 nowMs = WorldTimer::getMSTime();
    std::vector<std::string> events;
    std::vector<runtactics::Foe> foes;
    std::vector<std::pair<uint32, Unit*>> casting;
    {
        std::lock_guard<std::mutex> guard(lock);
        Live& live = LiveOf(leader, view);
        for (Creature* creature : creatures)
        {
            foes.push_back(FoeOf(live, creature, healers, nowMs, events));
            if (runtactics::ToInterrupt(foes.back()))
                casting.push_back({ foes.back().guid, creature });
        }
    }
    // The members' interrupts are judged through their own AIs, with no lock held.
    std::map<uint32, std::string> names;
    const std::vector<runtactics::Member> members = Members(leader, view, casting, names);
    runtactics::Plan plan = runtactics::Decide(foes, members);
    plan.at = int64_t(nowMs);

    ObjectGuid skull, cross, hadSkull, hadCross;
    std::string detail;
    bool told = false;
    bool routed = false;    // the interrupts differ from the kept plan's
    {
        std::lock_guard<std::mutex> guard(lock);
        Live& live = LiveOf(leader, view);
        const std::vector<runtactics::Pair>& had = live.plan.interrupts;
        routed = had.size() != plan.interrupts.size() || !std::equal(had.begin(), had.end(), plan.interrupts.begin(),
            [](const runtactics::Pair& a, const runtactics::Pair& b) { return a.member == b.member && a.foe == b.foe; });
        live.plan = plan;
        live.planned.clear();
        for (const runtactics::Foe& foe : foes)
            live.planned.push_back(foe.guid);
        live.stamp = view.stamp;
        ++live.decided;
        auto guidOf = [&live](uint32 id) { auto it = live.guids.find(id); return id && it != live.guids.end() ? it->second : ObjectGuid(); };
        skull = guidOf(plan.skull);
        cross = guidOf(plan.cross);
        hadSkull = live.skull;
        hadCross = live.cross;
        live.skull = skull;
        live.cross = cross;
        // What the plan says, told when it changes: the marks, the tank's target, and who interrupts whom.
        std::string interrupts;
        for (const runtactics::Pair& pair : plan.interrupts)
            interrupts += (interrupts.empty() ? "" : ",") + names[pair.member] + ":" + FoeText(live, pair.foe);
        const std::string marks = "marks=skull:" + FoeText(live, plan.skull) + ",cross:" + FoeText(live, plan.cross)
            + " tank=" + FoeText(live, plan.tank) + (interrupts.empty() ? "" : " interrupts=" + interrupts);
        if (marks != live.marks)
        {
            live.marks = marks;
            told = true;
            detail = Tag(live) + " " + marks + " why=" + why + " foes=" + std::to_string(foes.size())
                + " casts=" + std::to_string(plan.casts.size());
        }
    }
    // The marks, with no lock held: each icon set only when it changes, and one the plan set and no longer wants cleared
    // only while it still stands as the plan set it, so a mark of a person's is never taken off.
    auto mark = [group](uint8 icon, ObjectGuid want, ObjectGuid had)
    {
        const ObjectGuid now = group->GetTargetIcon(icon);
        if (!want.IsEmpty())
        {
            if (now == want)
                return false;
            group->SetTargetIcon(icon, want);
            return true;
        }
        if (had.IsEmpty() || now != had)
            return false;
        group->SetTargetIcon(icon, ObjectGuid());
        return true;
    };
    const bool markedSkull = mark(ICON_SKULL, skull, hadSkull);
    const bool markedCross = mark(ICON_CROSS, cross, hadCross);

    const uint32 leaderGuid = leader->GetGUIDLow();
    for (const std::string& event : events)
        sGuildBotMgr.RecordEvent(leaderGuid, "run_tactic", event);
    if (told)
        sGuildBotMgr.RecordEvent(leaderGuid, "run_tactic", detail);
    // The leader's action for this tick only when the party has something new to act on; a decision that changed nothing,
    // as a retry no member can take yet, lets the engine go on to the leader's own next action.
    return markedSkull || markedCross || told || routed;
}

bool GuildTactics::Clear(Player* leader, const GuildRunView& view, const std::string& why)
{
    if (!Settled(leader))
        return false;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = lives.find(view.instanceId);
        if (it == lives.end() || (it->second.skull.IsEmpty() && it->second.cross.IsEmpty()))
            return false;
    }
    return Decide(leader, view, why);
}

bool GuildTactics::Assigned(Player* bot)
{
    if (!Settled(bot))
        return false;
    std::lock_guard<std::mutex> guard(lock);
    auto it = lives.find(bot->GetInstanceId());
    return it != lives.end() && runtactics::InterruptFor(it->second.plan, bot->GetGUIDLow()) != 0;
}

bool GuildTactics::InterruptDue(Player* bot, const GuildRunView& view, Unit*& foe, uint32& spell)
{
    foe = nullptr;
    spell = 0;
    if (!Settled(bot) || !bot->IsAlive() || !view.valid || view.mode != GuildRunMode::Lead)
        return false;
    ObjectGuid target;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = lives.find(view.instanceId);
        if (it == lives.end())
            return false;
        const Live& live = it->second;
        if (!runtactics::PlanCurrent(live.stamp.generation, live.stamp.plan, view.stamp.generation, view.stamp.plan,
                view.stamp.generation, view.stamp.plan))
            return false;
        const uint32 id = runtactics::InterruptFor(live.plan, bot->GetGUIDLow());
        auto guid = live.guids.find(id);
        if (!id || guid == live.guids.end())
            return false;
        target = guid->second;
    }
    Creature* creature = bot->GetMap()->GetAnyTypeCreature(target);
    if (!creature || !creature->IsInWorld() || !creature->IsAlive())
        return false;
    uint8 kind = 0;
    Spell* cast = CastOf(creature, kind);
    if (!cast || !cast->IsInterruptible())
        return false;
    spell = Castable(bot, InterruptsOf(bot), creature);
    if (!spell)
        return false;
    foe = creature;
    return true;
}

bool GuildTactics::Interrupt(Player* bot, const GuildRunView& view, const GuildRunStamp& stamp)
{
    if (!Settled(bot) || !view.valid || view.mode != GuildRunMode::Lead || !runrules::Same(view.stamp, stamp))
        return false;
    // The plan's stamp, the run's and the action's: a plan of another revision does nothing.
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = lives.find(view.instanceId);
        if (it == lives.end() || !runtactics::PlanCurrent(it->second.stamp.generation, it->second.stamp.plan,
                view.stamp.generation, view.stamp.plan, stamp.generation, stamp.plan))
            return false;
    }
    Unit* foe = nullptr;
    uint32 spell = 0;
    if (!InterruptDue(bot, view, foe, spell))
        return false;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (!ai)
        return false;
    uint8 kind = 0;
    Spell* cast = CastOf(foe, kind);
    const uint32 castSpell = cast ? cast->m_spellInfo->Id : 0;
    // It faces the caster, so the interrupt goes now rather than after a turn. Its own cast, if any, gives way only once
    // the interrupt starts (Unit::SetCurrentCastedSpell), so a heal is never cut for an interrupt that does not go.
    if (!sServerFacade.IsInFront(bot, foe, sPlayerbotAIConfig.sightDistance, CAST_ANGLE_IN_FRONT))
        sServerFacade.SetFacingTo(bot, foe, true);
    const bool castNow = ai->CastSpell(spell, foe, nullptr, false);
    uint8 kindAfter = 0;
    Spell* after = foe->IsAlive() ? CastOf(foe, kindAfter) : nullptr;
    const bool still = after && after == cast && after->m_spellInfo->Id == castSpell;
    const runtactics::Outcome outcome = runtactics::Judge(castNow, still);

    std::string detail;
    uint32 leaderGuid = 0;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = lives.find(view.instanceId);
        if (it == lives.end() || !runrules::Same(it->second.stamp, stamp))
            return castNow;
        Live& live = it->second;
        leaderGuid = live.leader;
        auto known = live.known.find(foe->GetObjectGuid());
        const uint32 id = known != live.known.end() ? known->second.id : 0;
        const std::string what = std::string(bot->GetName()) + ":" + FoeText(live, id) + ":" + runtactics::OutcomeName(outcome);
        if (outcome == runtactics::Outcome::NotCast)
        {
            // Not cast: the AI turns or waits, and tries again on its next look. Said once a cast.
            const std::string once = what + ":" + std::to_string(castSpell) + ":" + std::to_string(known != live.known.end() ? known->second.castStartMs : 0);
            if (live.notCast.insert(once).second)
                detail = Tag(live) + " interrupt=" + what + " spell=" + std::to_string(castSpell) + " by=" + std::to_string(spell);
        }
        else
        {
            runtactics::Spent(live.plan, bot->GetGUIDLow(), id);
            if (known != live.known.end())
            {
                ++known->second.tried;
                if (outcome == runtactics::Outcome::Interrupted)
                    known->second.interrupted = true;
            }
            detail = Tag(live) + " interrupt=" + what + " spell=" + std::to_string(castSpell) + " by=" + std::to_string(spell);
        }
    }
    if (!detail.empty() && leaderGuid)
        sGuildBotMgr.RecordEvent(leaderGuid, "run_tactic", detail);
    return castNow;
}

ObjectGuid GuildTactics::TankPickOf(Player* bot, const GuildRunView& view, uint32& id, runtactics::Victim& on)
{
    id = 0;
    on = runtactics::Victim::None;
    if (!Settled(bot) || !view.valid || view.mode != GuildRunMode::Lead || !view.controls || !PlayerbotAI::IsTank(bot))
        return ObjectGuid();
    // The kept plan's kill order and its creatures, its skull and this tank's last pick, copied under the lock.
    std::vector<uint32> order;
    std::vector<ObjectGuid> guids;
    uint32 skull = 0, last = 0;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = lives.find(view.instanceId);
        if (it == lives.end() || !runrules::Same(it->second.stamp, view.stamp))
            return ObjectGuid();
        const Live& live = it->second;
        skull = live.plan.skull;
        auto picked = live.tankPicks.find(bot->GetGUIDLow());
        last = picked == live.tankPicks.end() ? 0 : picked->second;
        for (uint32 foe : live.plan.order)
        {
            auto guid = live.guids.find(foe);
            if (guid == live.guids.end())
                continue;
            order.push_back(foe);
            guids.push_back(guid->second);
        }
    }
    // Whom each is on now. Once the tank has taunted it, it is on the tank and the pick is kept while it lives and stays
    // in the order on the tank or a member, unless another goes on the healer; a skull taken off the healer lets go, and
    // the tank takes the mark and the loose creatures again, though the plan is not decided again for it.
    std::vector<std::pair<uint32_t, runtactics::Victim>> victims;
    Map* map = bot->GetMap();
    for (size_t i = 0; i < order.size(); ++i)
    {
        Creature* creature = map->GetAnyTypeCreature(guids[i]);
        if (creature && creature->IsInWorld() && creature->IsAlive())
            victims.push_back({ order[i], VictimOf(creature, bot) });
    }
    id = runtactics::TankPick(order, victims, skull, last);
    if (!id)
        return ObjectGuid();
    for (const auto& victim : victims)
        if (victim.first == id)
            on = victim.second;
    return guids[std::find(order.begin(), order.end(), id) - order.begin()];
}

ObjectGuid GuildTactics::TankTarget(Player* bot, const GuildRunView& view)
{
    uint32 id = 0;
    runtactics::Victim on = runtactics::Victim::None;
    const ObjectGuid target = TankPickOf(bot, view, id, on);
    if (!Settled(bot) || !view.valid || view.mode != GuildRunMode::Lead || !view.controls || !PlayerbotAI::IsTank(bot))
        return target;
    // A change of pick, told once under the leader: tank_pick=<foe> on=healer|member, or tank_pick=none. A pick kept
    // as it goes on the tank is no change.
    std::string detail;
    uint32 leaderGuid = 0;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = lives.find(view.instanceId);
        if (it == lives.end() || !runrules::Same(it->second.stamp, view.stamp))
            return target;
        Live& live = it->second;
        uint32& last = live.tankPicks[bot->GetGUIDLow()];
        if (last != id)
        {
            last = id;
            leaderGuid = live.leader;
            detail = Tag(live) + " tank_pick=" + (id ? FoeText(live, id) + " on=" + VictimName(on) : std::string("none"));
        }
    }
    if (!detail.empty() && leaderGuid)
        sGuildBotMgr.RecordEvent(leaderGuid, "run_tactic", detail);
    return target;
}

std::vector<std::tuple<uint32, std::string, std::string>> GuildTactics::Sweep()
{
    std::vector<std::tuple<uint32, std::string, std::string>> events;
    struct Release
    {
        uint32 group = 0;
        ObjectGuid skull, cross;
    };
    std::vector<Release> releases;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (auto it = lives.begin(); it != lives.end();)
        {
            Live& live = it->second;
            GuildRunMode mode = GuildRunMode::Ended;
            GuildRunStamp stamp;
            uint32 leader = 0;
            const bool found = sGuildRunStates.ModeOf(live.instanceId, mode, stamp, leader);
            const char* why = !found ? "no_run" : mode != GuildRunMode::Lead ? runrules::ModeName(mode)
                : leader != live.leader ? "leader" : live.stamp.generation && stamp.generation != live.stamp.generation ? "generation" : nullptr;
            if (!why)
            {
                ++it;
                continue;
            }
            releases.push_back({ live.group, live.skull, live.cross });
            std::string open;
            for (auto const& [guid, known] : live.known)
                if (known.castSpell)
                    open += (open.empty() ? "" : ",") + FoeText(live, known.id) + ":" + std::to_string(known.castSpell);
            events.emplace_back(live.leader, "run_tactic", Tag(live) + " result=released why=" + why + " decided="
                + std::to_string(live.decided) + (open.empty() ? "" : " open_casts=" + open));
            it = lives.erase(it);
        }
    }
    // The marks the plans set, where they still stand: never a mark someone else has set since.
    for (const Release& release : releases)
        if (Group* group = sObjectMgr.GetGroupById(release.group))
        {
            if (!release.skull.IsEmpty() && group->GetTargetIcon(ICON_SKULL) == release.skull)
                group->SetTargetIcon(ICON_SKULL, ObjectGuid());
            if (!release.cross.IsEmpty() && group->GetTargetIcon(ICON_CROSS) == release.cross)
                group->SetTargetIcon(ICON_CROSS, ObjectGuid());
        }
    return events;
}

std::list<std::string> GuildTactics::Describe(const std::string& leaderName)
{
    std::list<std::string> out;
    {
        std::lock_guard<std::mutex> guard(lock);
        for (auto const& [instance, live] : lives)
        {
            std::ostringstream line;
            line << "instance " << instance << " leader " << live.leader << " " << Tag(live) << " decided=" << live.decided
                << " skull=" << FoeText(live, live.plan.skull) << " cross=" << FoeText(live, live.plan.cross)
                << " tank=" << FoeText(live, live.plan.tank) << " order=" << live.plan.order.size()
                << " casts=" << live.plan.casts.size() << " interrupts=" << live.plan.interrupts.size();
            out.push_back(line.str());
        }
    }
    if (out.empty())
        out.push_back("no tactics plans kept");
    if (leaderName.empty())
        return out;

    Player* leader = ObjectAccessor::FindPlayerByName(leaderName.c_str());
    if (!Settled(leader))
        return { leaderName + " is not in the world" };
    const GuildRunView view = sGuildRunStates.View(leader);
    if (!view.valid || !view.lead)
        return { leaderName + " leads no run here" };
    uint32 passed = 0, failed = 0;
    auto check = [&](const std::string& what, bool ok, const std::string& why)
    {
        ok ? ++passed : ++failed;
        out.push_back(std::string(ok ? "pass " : "fail ") + what + "  " + why);
    };

    // The facts, from a copy of what the plan has seen, so that nothing kept changes: the fight's creatures, else the
    // stop's pack alive.
    const std::vector<Player*> party = PartyHere(leader, view);
    const std::vector<Player*> healers = Healers(party);
    std::vector<Creature*> creatures = FightCreatures(leader, view, nullptr);
    if (creatures.empty())
        for (uint32 spawn : view.stop.spawns)
            if (Creature* creature = leader->GetMap()->GetCreature(spawn))
                if (creature->IsAlive() && creature->IsInWorld())
                    creatures.push_back(creature);
    Live copy;
    {
        std::lock_guard<std::mutex> guard(lock);
        auto it = lives.find(view.instanceId);
        if (it != lives.end())
            copy = it->second;
    }
    const Live kept = copy;
    std::vector<std::string> ignored;
    std::vector<runtactics::Foe> foes;
    std::vector<std::pair<uint32, Unit*>> casting;
    for (Creature* creature : creatures)
    {
        foes.push_back(FoeOf(copy, creature, healers, WorldTimer::getMSTime(), ignored));
        const runtactics::Foe& foe = foes.back();
        const Abilities& abilities = copy.known[creature->GetObjectGuid()].abilities;
        uint8 kind = 0;
        Spell* cast = CastOf(creature, kind);
        std::ostringstream line;
        line << "foe " << FoeText(copy, foe.guid) << " " << creature->GetName() << " type=" << int(foe.type)
            << " elite=" << foe.elite << " hp=" << foe.health << " summons=" << foe.summons << " heals=" << foe.heals
            << " flees=" << foe.flees << " from=" << (abilities.from.empty() ? "seen_or_none" : abilities.from)
            << " fleeing=" << foe.fleeing << " on_healer=" << foe.onHealer << " in_fight=" << foe.inFight;
        if (cast)
            line << " casting=" << cast->m_spellInfo->Id << ":" << KindName(kind) << " interruptible=" << cast->IsInterruptible();
        out.push_back(line.str());
        if (runtactics::ToInterrupt(foe))
            casting.push_back({ foe.guid, creature });
    }
    std::map<uint32, std::string> names;
    const std::vector<runtactics::Member> members = Members(leader, view, casting, names);
    for (Player* member : party)
    {
        std::string spells;
        for (uint32 spell : InterruptsOf(member))
            if (const SpellEntry* entry = sServerFacade.LookupSpellInfo(spell))
                spells += (spells.empty() ? "" : ",") + std::to_string(spell) + ":" + entry->SpellName[0];
        out.push_back("member " + std::string(member->GetName()) + " engine=" + (EnginePlays(member, view) ? "1" : "0")
            + " role=" + std::to_string(int(RoleOf(member))) + " interrupts=" + (spells.empty() ? "none" : spells));
    }
    const runtactics::Plan fresh = runtactics::Decide(foes, members);
    out.push_back("fresh skull=" + FoeText(copy, fresh.skull) + " cross=" + FoeText(copy, fresh.cross) + " tank=" + FoeText(copy, fresh.tank)
        + " interrupts=" + std::to_string(fresh.interrupts.size()) + " casts=" + std::to_string(fresh.casts.size()));

    // Checks. The marks stand as the kept plan set them, when it is the run's plan now.
    Group* group = leader->GetGroup();
    const bool current = kept.stamp.generation && runrules::Same(kept.stamp, view.stamp);
    auto keptGuid = [&kept](uint32 id) { auto it = kept.guids.find(id); return id && it != kept.guids.end() ? it->second : ObjectGuid(); };
    if (current && group)
        check("marks", group->GetTargetIcon(ICON_SKULL) == keptGuid(kept.plan.skull) && group->GetTargetIcon(ICON_CROSS) == keptGuid(kept.plan.cross),
            "skull=" + FoeText(kept, kept.plan.skull) + " cross=" + FoeText(kept, kept.plan.cross));
    else
        out.push_back("skip marks  no plan under the run's stamp " + runrules::StampText(view.stamp));
    // Every icon the plan set stands on a creature in a fight, or on none: no mark goes on a creature before it is in the
    // fight, at the pull or after it (FightCreatures).
    {
        std::string standing, outside;
        for (const auto& [name, guid] : { std::make_pair("skull", kept.skull), std::make_pair("cross", kept.cross) })
        {
            Creature* creature = guid.IsEmpty() ? nullptr : leader->GetMap()->GetAnyTypeCreature(guid);
            const bool alive = creature && creature->IsInWorld() && creature->IsAlive();
            standing += (standing.empty() ? "" : " ") + std::string(name) + "=" + (alive ? std::to_string(guid.GetEntry()) + "."
                + std::to_string(guid.GetCounter()) + (creature->IsInCombat() ? ":in_fight" : ":out") : std::string("none"));
            if (alive && !creature->IsInCombat())
                outside += (outside.empty() ? "" : ",") + std::string(name);
        }
        check("marks_in_fight", outside.empty(), standing + (outside.empty() ? "" : ", out of a fight: " + outside));
    }

    // The kept plan read under a run one revision older, as a member's trigger and the tank's value read it: no member's
    // interrupt is due, and no tank is given a target. Reads only.
    const std::vector<Player*> tanks = [&party]()
    {
        std::vector<Player*> found;
        for (Player* member : party)
            if (PlayerbotAI::IsTank(member))
                found.push_back(member);
        return found;
    }();
    if (current)
    {
        GuildRunView older = view;
        older.stamp.plan = view.stamp.plan ? view.stamp.plan - 1 : view.stamp.plan + 1;
        uint32 assigned = 0, due = 0;
        for (Player* member : party)
        {
            if (runtactics::InterruptFor(kept.plan, member->GetGUIDLow()))
                ++assigned;
            Unit* foe = nullptr;
            uint32 spell = 0;
            if (InterruptDue(member, older, foe, spell))
                ++due;
        }
        uint32 tankTargets = 0;
        for (Player* tank : tanks)
        {
            GuildRunView tankOlder = sGuildRunStates.View(tank);
            tankOlder.stamp = older.stamp;
            if (!TankTarget(tank, tankOlder).IsEmpty())
                ++tankTargets;
        }
        check("stale_refused", !due && !tankTargets, "under " + runrules::StampText(older.stamp) + ": " + std::to_string(due) + " of "
            + std::to_string(assigned) + " assigned due, " + std::to_string(tankTargets) + " tank targets");
    }
    else
        out.push_back("skip stale_refused  no plan under the run's stamp " + runrules::StampText(view.stamp));

    // A person's character is never given an interrupt: each member of a pair, kept or fresh, checked on its own against
    // the run's persons and its AI, not through the flags the decision filtered on.
    Map* map = leader->GetMap();
    auto playerOf = [map](uint32 guid) { return map->GetPlayer(ObjectGuid(HIGHGUID_PLAYER, guid)); };
    uint32 pairs = 0, persons = 0;
    std::string personNames;
    auto person = [&](uint32 guid)
    {
        ++pairs;
        Player* member = playerOf(guid);
        PlayerbotAI* ai = member ? member->GetPlayerbotAI() : nullptr;
        const bool listed = std::find(view.persons.begin(), view.persons.end(), guid) != view.persons.end();
        if (member && ai && !ai->IsRealPlayer() && !listed)
            return;
        ++persons;
        personNames += (personNames.empty() ? "" : ",") + (member ? std::string(member->GetName()) : std::to_string(guid));
    };
    if (current)
        for (const runtactics::Pair& pair : kept.plan.interrupts)
            person(pair.member);
    for (const runtactics::Pair& pair : fresh.interrupts)
        person(pair.member);
    check("interrupts_engine_only", !persons, std::to_string(pairs) + " pairs, kept and fresh" + (personNames.empty() ? "" : ", persons " + personNames));

    // Every interrupt the kept plan gives, on a caster still casting a cast to interrupt, is one the member's own AI can
    // cast at it now. A pair tried is gone from the plan, so a fail here is a pair the member cannot take.
    if (current)
    {
        uint32 checked = 0;
        std::string cannot;
        for (const runtactics::Pair& pair : kept.plan.interrupts)
        {
            Player* member = playerOf(pair.member);
            Creature* caster = map->GetAnyTypeCreature(keptGuid(pair.foe));
            uint8 kind = 0;
            Spell* cast = caster && caster->IsInWorld() && caster->IsAlive() ? CastOf(caster, kind) : nullptr;
            if (!member || !cast || !cast->IsInterruptible())
                continue;
            ++checked;
            if (!Castable(member, InterruptsOf(member), caster))
                cannot += (cannot.empty() ? "" : ",") + std::string(member->GetName()) + ":" + FoeText(kept, pair.foe);
        }
        check("interrupts_castable", cannot.empty(), std::to_string(checked) + " of " + std::to_string(kept.plan.interrupts.size())
            + " kept pairs on a cast going" + (cannot.empty() ? "" : ", cannot " + cannot));
    }
    else
        out.push_back("skip interrupts_castable  no plan under the run's stamp " + runrules::StampText(view.stamp));

    // The party's tank, asked as TankTargetValue asks, without telling a pick: runtactics::TankPick over the kept plan's
    // kill order, its skull, the tank's last pick and whom its living creatures are on now: the healer first, then the
    // last pick while on the tank or a member, then the first on a member who is not a tank, the skull left out; nothing
    // otherwise. A creature under crowd control that damage breaks is on nobody.
    if (!current)
        out.push_back("skip tank_target  no plan under the run's stamp " + runrules::StampText(view.stamp));
    else if (tanks.empty())
        out.push_back("skip tank_target  no tank in the party");
    for (Player* tank : current ? tanks : std::vector<Player*>())
    {
        const GuildRunView tankView = sGuildRunStates.View(tank);
        uint32 gotId = 0;
        runtactics::Victim gotOn = runtactics::Victim::None;
        const ObjectGuid got = TankPickOf(tank, tankView, gotId, gotOn);
        const bool runs = tankView.valid && tankView.controls && tankView.mode == GuildRunMode::Lead && runrules::Same(tankView.stamp, kept.stamp);
        std::vector<std::pair<uint32_t, runtactics::Victim>> victims;
        for (uint32 foe : kept.plan.order)
        {
            const ObjectGuid guid = keptGuid(foe);
            Creature* creature = guid.IsEmpty() ? nullptr : map->GetAnyTypeCreature(guid);
            if (creature && creature->IsInWorld() && creature->IsAlive())
                victims.push_back({ foe, VictimOf(creature, tank) });
        }
        auto picked = kept.tankPicks.find(tank->GetGUIDLow());
        const uint32 last = picked == kept.tankPicks.end() ? 0 : picked->second;
        const uint32 pick = runs ? runtactics::TankPick(kept.plan.order, victims, kept.plan.skull, last) : 0;
        runtactics::Victim on = runtactics::Victim::None;
        for (const auto& victim : victims)
            if (pick && victim.first == pick)
                on = victim.second;
        const ObjectGuid expected = keptGuid(pick);
        check("tank_target", got == expected, std::string(tank->GetName()) + " pick=" + FoeText(kept, pick)
            + (pick ? std::string(" on_") + VictimName(on) + (pick == last ? " kept" : "") : std::string(" none"))
            + (runs ? "" : " run_not_current") + " got=" + (got.IsEmpty() ? std::string("none") : std::to_string(got.GetCounter())));
    }
    out.push_back(std::to_string(passed) + " passed, " + std::to_string(failed) + " failed");
    return out;
}
