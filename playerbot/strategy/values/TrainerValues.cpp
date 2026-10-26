
#include "playerbot/playerbot.h"
#include "TrainerValues.h"
#include "SharedValueContext.h"
#include "playerbot/PlayerbotHelpMgr.h"

using namespace ai;


trainableSpellMap* TrainableSpellMapValue::Calculate()
{
    trainableSpellMap* spellMap = new trainableSpellMap;

    //           template, trainer
    std::unordered_map <uint32, std::vector<CreatureInfo const*>> trainerTemplateIds;

    //Select all trainer lists and their trainers.
    for (uint32 id = 0; id < sCreatureStorage.GetMaxEntry(); ++id)
    {
        CreatureInfo const* creatureInfo = sCreatureStorage.LookupEntry<CreatureInfo>(id);
        if (!creatureInfo)
            continue;

        if (!creatureInfo->TrainerType && !creatureInfo->TrainerClass)
            continue;

        if(creatureInfo->TrainerTemplateId)
            trainerTemplateIds[creatureInfo->TrainerTemplateId].push_back(creatureInfo);
        else
            trainerTemplateIds[id].push_back(creatureInfo);
    }

    for (auto& [templateOrEntryId, trainers] : trainerTemplateIds)
    {
        TrainerSpellData const* trainer_spells = sObjectMgr.GetNpcTrainerTemplateSpells(templateOrEntryId);
        if (!trainer_spells)
            trainer_spells = sObjectMgr.GetNpcTrainerSpells(templateOrEntryId);

        if (!trainer_spells)
            continue;

        CreatureInfo const* firstTrainer = trainers.front();

        TrainerType trainerType = (TrainerType)firstTrainer->TrainerType;

        uint32 spellRequirement;
        if (trainerType == TRAINER_TYPE_CLASS || trainerType == TRAINER_TYPE_PETS)
            spellRequirement = firstTrainer->TrainerClass;
        else if (trainerType == TRAINER_TYPE_MOUNTS)
            spellRequirement = firstTrainer->TrainerRace;

        for (auto& [id, trainerSpell] : trainer_spells->spellList)
        {
            const TrainerSpell* sameTrainerSpell = &trainerSpell;
            for (auto& [otherTrainerSpell, trainers] : (*spellMap)[trainerType][spellRequirement])
            {
                if (otherTrainerSpell->spell != trainerSpell.spell)
                    continue;

                if (otherTrainerSpell->spellCost != trainerSpell.spellCost)
                    continue;

                if (otherTrainerSpell->reqSkill != trainerSpell.reqSkill)
                    continue;

                if (otherTrainerSpell->reqSkillValue != trainerSpell.reqSkillValue)
                    continue;

                if (otherTrainerSpell->reqLevel != trainerSpell.reqLevel)
                    continue;

#ifndef MANGOSBOT_TWO
                if (otherTrainerSpell->learnedSpell != trainerSpell.learnedSpell)
#else
                if (otherTrainerSpell->learnedSpell[0] != trainerSpell.learnedSpell[0])
#endif
                    continue;

                if (otherTrainerSpell->conditionId != trainerSpell.conditionId)
                    continue;

                sameTrainerSpell = otherTrainerSpell;
                break;
            }

            if (trainerType == TRAINER_TYPE_TRADESKILLS)
            {
                if (trainerSpell.reqSkill)
                    spellRequirement = trainerSpell.reqSkill;
                else
                {
                    // exist, already checked at loading
#ifdef MANGOSBOT_ZERO
                    SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(trainerSpell.learnedSpell);
#else
                    SpellEntry const* spell = sSpellTemplate.LookupEntry<SpellEntry>(trainerSpell.learnedSpell[0]);
#endif

                    spellRequirement = spell->EffectMiscValue[1];
                }
            }

            for (auto& trainer : trainers)
                (*spellMap)[trainerType][spellRequirement][sameTrainerSpell].push_back(trainer->Entry);
        }
    }

    return spellMap;
}

// The bot's race and class can have every skill the trainer spell gives: the skill lines of the spell it casts and
// of the spell it teaches, as SkillLineAbility.dbc lists them with their own race and class masks, and the skills
// their skill step effects raise. Lockpicking's teaching spell is only in the second kind: its skill step raises a
// rogue-only skill, and the Pick Lock it teaches is in no skill line.
bool TrainableSpellsValue::CanHaveSkillsOf(TrainerSpell const* trainerSpell) const
{
    auto canHaveSkill = [&](uint32 skillId)
    {
        auto skillInfo = sSpellMgr.GetSkillRaceClassInfoMapBounds(skillId);
        return skillInfo.first == skillInfo.second || bot->GetSkillInfo(skillId);
    };

#ifdef MANGOSBOT_TWO
    uint32 spells[] = { trainerSpell->spell, trainerSpell->learnedSpell[0] };
#else
    uint32 spells[] = { trainerSpell->spell, trainerSpell->learnedSpell };
#endif
    for (uint32 spellId : spells)
    {
        auto abilities = sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(spellId);
        for (auto itr = abilities.first; itr != abilities.second; ++itr)
        {
            SkillLineAbilityEntry const* ability = itr->second;
            if (ability->racemask && !(ability->racemask & bot->getRaceMask()))
                return false;
            if (ability->classmask && !(ability->classmask & bot->getClassMask()))
                return false;
            if (!canHaveSkill(ability->skillId))
                return false;
        }

        SpellEntry const* spell = sServerFacade.LookupSpellInfo(spellId);
        if (!spell)
            continue;
        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (spell->Effect[i] == SPELL_EFFECT_SKILL_STEP && !canHaveSkill(spell->EffectMiscValue[i]))
                return false;
    }
    return true;
}

std::vector<TrainerSpell const*> TrainableSpellsValue::Calculate()
{
    std::vector<TrainerSpell const*> trainableSpells;

    int8 qualifierType = getQualifier().empty() ? -1 : stoi(getQualifier());

    trainableSpellMap* spellMap = GAI_VALUE(trainableSpellMap*, "trainable spell map");

    for (auto& [trainerType, spellReqList] : *spellMap)
    {
        if (trainerType >= 0 && trainerType != qualifierType)
            continue;

        for (auto& [requirement, trainerSpellList] : spellReqList)
        {
            if (trainerType == TRAINER_TYPE_CLASS && requirement != bot->getClass())
                continue;
            if (trainerType == TRAINER_TYPE_MOUNTS && requirement != bot->getRace())
                continue;

            for (auto& [trainerSpell, trainers] : trainerSpellList)
            {
                uint32 reqLevel = 0;

                reqLevel = trainerSpell->isProvidedReqLevel ? trainerSpell->reqLevel : std::max(reqLevel, trainerSpell->reqLevel);
                TrainerSpellState state = bot->GetTrainerSpellState(trainerSpell, reqLevel);
                if (state != TRAINER_SPELL_GREEN)
                    continue;

                // wow-playground: some trade trainer spells teach a skill of another race or class, and the trainer
                // state does not check that. Lockpicking's quest NPCs teach it for a copper to anyone, so every level 3
                // mage had a spell that had "waited two levels" and walked to Stormwind's weapon master for it.
                if (trainerType == TRAINER_TYPE_TRADESKILLS && !CanHaveSkillsOf(trainerSpell))
                    continue;

                //Skip initial profession training.
#ifdef MANGOSBOT_ZERO
                if (bot->GetLevel() < 10 && sSpellMgr.IsProfessionSpell(trainerSpell->learnedSpell) && sSpellMgr.GetSpellRank(trainerSpell->learnedSpell) == 1)
#else
                if (bot->GetLevel() < 10 && sSpellMgr.IsProfessionSpell(trainerSpell->learnedSpell[0]) && sSpellMgr.GetSpellRank(trainerSpell->learnedSpell[0]) == 1)
#endif
                    continue;

                trainableSpells.push_back(trainerSpell);
            }
        }
    }   

    return trainableSpells;
}

std::string TrainableSpellsValue::Format()
{
    std::vector<std::string> vec;  
    for (auto t : value) {
        SpellEntry const* spell = sServerFacade.LookupSpellInfo(t->spell);
        if (!spell)
            continue;
        vec.push_back(chat->formatSpell(spell));
    } 
    
    return sPlayerbotHelpMgr.makeList(vec, "[<part>]");
}

std::vector<int32> AvailableTrainersValue::Calculate()
{
    std::vector<TrainerSpell const*> trainableSpells = AI_VALUE2(std::vector<TrainerSpell const*>, "trainable spells", getQualifier());;
    std::vector<int32> retTrainers;

    int8 qualifierType = getQualifier().empty() ? -1 : stoi(getQualifier());

    trainableSpellMap* spellMap = GAI_VALUE(trainableSpellMap*, "trainable spell map");

    for (auto& [trainerType, spellReqList] : *spellMap)
    {
        if (trainerType >= 0 && trainerType != qualifierType)
            continue;

        for (auto& [requirement, trainerSpellList] : spellReqList)
        {
            if (trainerType == TRAINER_TYPE_CLASS && requirement != bot->getClass())
                continue;
            if (trainerType == TRAINER_TYPE_MOUNTS && requirement != bot->getRace())
                continue;

            for (auto& [trainerSpell, trainers] : trainerSpellList)
            {
                if (std::find(trainableSpells.begin(), trainableSpells.end(), trainerSpell) == trainableSpells.end())
                    continue;

                for (auto& trainer : trainers)
                {
                    if(std::find(retTrainers.begin(), retTrainers.end(), trainer) == retTrainers.end())
                        retTrainers.push_back(trainer);
                }
            }
        }
    }

    return retTrainers;
}

uint32 TrainCostValue::Calculate()
{
    uint32 TotalCost = 0;

    for (auto& spells : AI_VALUE2(std::vector<TrainerSpell const*>, "trainable spells", getQualifier()))
        TotalCost += spells->spellCost;
   
    return TotalCost;
}
