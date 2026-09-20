/*
 * mod-multi-imp
 *
 * Lets a warlock have more than one imp. The real imp stays the one and only
 * "Pet" (pet bar, saved in the DB, Master Demonologist, ...). Every cast of
 * Summon Imp additionally spawns N extra imps as plain guardians. They mirror
 * the state of the real imp: react state (aggressive / defensive / passive),
 * follow / stay, and its attack target. Guardians get no PetAI from the core,
 * so Firebolt is cast from here.
 *
 * Why guardians: Unit::SetMinion / Player::SummonPet only allow one Pet per
 * owner. SummonProperties 61 (Category ALLY, Type GUARDIAN, verified in
 * SummonProperties.dbc) is NOT a PET category, so Minion::IsGuardianPet() is
 * false and the core leaves the real pet slot alone.
 */

#include "Config.h"
#include "DBCStores.h"
#include "Map.h"
#include "Pet.h"
#include "PetDefines.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <vector>

namespace
{
    constexpr uint32 SPELL_SUMMON_IMP = 688;
    // NPC_IMP (416) now comes from the core's NPCEntries enum in PetDefines.h (included above);
    // a local copy here would be an ambiguous symbol, so we use the core one.

    // Category ALLY, Type GUARDIAN, Slot 0. Must not be a PET category, see file header.
    constexpr uint32 SUMMON_PROPERTIES_GUARDIAN = 61;

    // Firebolt ranks, highest first. The real imp's spellbook decides which one is used.
    constexpr std::array<uint32, 9> FIREBOLT_RANKS = { 47964, 27267, 11763, 11762, 7802, 7801, 7800, 7799, 3110 };
    constexpr float FIREBOLT_MAX_RANGE = 30.0f;
    constexpr float SUMMON_SCATTER_RADIUS = 3.0f;

    struct MultiImpConfig
    {
        bool enable = true;
        uint32 extraCount = 2;
        uint32 syncIntervalMs = 500;
        bool includeBots = false;
        bool castFirebolt = true;
    };

    MultiImpConfig sCfg;

    // Per-player state, lives in Player::CustomData (map updates run in several threads).
    struct MultiImpData : public DataMap::Base
    {
        bool wanted = false; // player summoned an imp and expects extras
        uint32 elapsed = 0;
    };

    constexpr char const* DATA_KEY = "MultiImp";

    std::vector<Creature*> CollectExtraImps(Player* owner)
    {
        std::vector<Creature*> result;
        for (Unit* unit : owner->m_Controlled)
            if (unit->IsCreature() && unit->GetEntry() == NPC_IMP && unit->IsSummon() && !unit->IsPet())
                result.push_back(unit->ToCreature());

        return result;
    }

    void DespawnExtras(Player* owner)
    {
        for (Creature* extra : CollectExtraImps(owner))
            extra->ToTempSummon()->UnSummon();
    }

    void SummonExtras(Player* owner, uint32 count)
    {
        SummonPropertiesEntry const* properties = sSummonPropertiesStore.LookupEntry(SUMMON_PROPERTIES_GUARDIAN);
        if (!properties)
        {
            LOG_ERROR("module.multiimp", "SummonProperties {} not found, cannot summon extra imps", SUMMON_PROPERTIES_GUARDIAN);
            return;
        }

        Map* map = owner->GetMap();
        if (!map)
            return;

        Position const center = owner->GetPosition();
        for (uint32 i = 0; i < count; ++i)
        {
            Position pos = owner->GetRandomPoint(center, SUMMON_SCATTER_RADIUS);

            // duration 0 => lives until it dies or we unsummon it
            TempSummon* summon = map->SummonCreature(NPC_IMP, pos, properties, 0, owner, SPELL_SUMMON_IMP);
            if (!summon)
                return;

            summon->SetFaction(owner->GetFaction());

            if (summon->HasUnitTypeMask(UNIT_MASK_MINION))
                static_cast<Minion*>(summon)->SetFollowAngle(owner->GetAbsoluteAngle(pos.GetPositionX(), pos.GetPositionY()));

            summon->GetMotionMaster()->Clear(false);
            summon->GetMotionMaster()->MoveFollow(owner, PET_FOLLOW_DIST, summon->GetFollowAngle(), MOTION_SLOT_ACTIVE);
        }
    }

    uint32 SelectFirebolt(Pet* mainPet)
    {
        for (uint32 spellId : FIREBOLT_RANKS)
            if (mainPet->HasSpell(spellId))
                return spellId;

        return FIREBOLT_RANKS.back();
    }

    Unit* SelectTarget(Player* owner, Pet* mainPet, ReactStates react)
    {
        if (react == REACT_PASSIVE)
            return nullptr;

        Unit* target = mainPet->GetVictim();
        if (!target)
            target = owner->GetVictim();

        if (target && target->IsAlive() && mainPet->IsValidAttackTarget(target))
            return target;

        return nullptr;
    }

    void SyncExtra(Creature* extra, Player* owner, Unit* target, ReactStates react, CommandStates command, uint32 fireboltId)
    {
        extra->SetReactState(react);

        if (react == REACT_PASSIVE)
        {
            if (extra->GetVictim() || extra->IsInCombat())
            {
                extra->AttackStop();
                extra->InterruptNonMeleeSpells(false);
            }
        }

        MotionMaster* motion = extra->GetMotionMaster();
        MovementGeneratorType const currentMotion = motion->GetCurrentMovementGeneratorType();

        if (!target)
        {
            if (extra->IsInCombat() || extra->HasUnitState(UNIT_STATE_CASTING))
                return;

            if (command == COMMAND_STAY)
            {
                if (currentMotion != IDLE_MOTION_TYPE)
                {
                    motion->Clear(false);
                    motion->MoveIdle();
                    extra->StopMoving();
                }
            }
            else if (currentMotion != FOLLOW_MOTION_TYPE)
            {
                motion->Clear(false);
                motion->MoveFollow(owner, PET_FOLLOW_DIST, extra->GetFollowAngle(), MOTION_SLOT_ACTIVE);
            }

            return;
        }

        if (extra->GetVictim() != target)
        {
            if (command == COMMAND_STAY)
                extra->SetTarget(target->GetGUID());
            else if (extra->AI())
                extra->AI()->AttackStart(target);
        }

        if (sCfg.castFirebolt && fireboltId && !extra->HasUnitState(UNIT_STATE_CASTING) &&
            extra->IsWithinDistInMap(target, FIREBOLT_MAX_RANGE) && extra->IsWithinLOSInMap(target))
            extra->CastSpell(target, fireboltId, false);
    }

    void TickPlayer(Player* owner, MultiImpData* data)
    {
        Pet* mainPet = owner->GetPet();
        if (!mainPet)
        {
            // dismissed, or mid-teleport: extras cannot outlive the real imp, they get topped up once it is back
            DespawnExtras(owner);
            return;
        }

        if (mainPet->GetEntry() != NPC_IMP)
        {
            // replaced by another demon: the feature is over until the next Summon Imp
            DespawnExtras(owner);
            data->wanted = false;
            return;
        }

        std::vector<Creature*> extras = CollectExtraImps(owner);

        // top up after combat (extras died, teleport, ...), never mid-fight so death is not free
        if (extras.size() < sCfg.extraCount && mainPet->IsAlive() && owner->IsAlive() && !owner->IsInCombat())
        {
            SummonExtras(owner, sCfg.extraCount - static_cast<uint32>(extras.size()));
            return;
        }

        if (extras.empty() || !mainPet->IsAlive())
            return;

        ReactStates const react = mainPet->GetReactState();

        CommandStates command = COMMAND_FOLLOW;
        if (CharmInfo* charmInfo = mainPet->GetCharmInfo())
            command = charmInfo->GetCommandState();

        Unit* target = SelectTarget(owner, mainPet, react);
        uint32 const fireboltId = SelectFirebolt(mainPet);

        for (Creature* extra : extras)
            if (extra->IsAlive())
                SyncExtra(extra, owner, target, react, command, fireboltId);
    }
}

class MultiImpWorldScript : public WorldScript
{
public:
    MultiImpWorldScript() : WorldScript("MultiImpWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sCfg.enable = sConfigMgr->GetOption<bool>("MultiImp.Enable", true);
        sCfg.extraCount = std::min<uint32>(sConfigMgr->GetOption<uint32>("MultiImp.ExtraCount", 2), 9);
        sCfg.syncIntervalMs = std::max<uint32>(sConfigMgr->GetOption<uint32>("MultiImp.SyncIntervalMs", 500), 100);
        sCfg.includeBots = sConfigMgr->GetOption<bool>("MultiImp.IncludeBots", false);
        sCfg.castFirebolt = sConfigMgr->GetOption<bool>("MultiImp.CastFirebolt", true);
    }
};

class MultiImpSpellScript : public AllSpellScript
{
public:
    MultiImpSpellScript() : AllSpellScript("MultiImpSpellScript", { ALLSPELLHOOK_ON_CAST }) { }

    void OnSpellCast(Spell* /*spell*/, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (!sCfg.enable || !spellInfo || spellInfo->Id != SPELL_SUMMON_IMP || !caster)
            return;

        Player* player = caster->ToPlayer();
        if (!player || (!sCfg.includeBots && player->GetSession()->IsBot()))
            return;

        // the real imp is created by the spell's own effect, which has already run at this point
        Pet* mainPet = player->GetPet();
        if (!mainPet || mainPet->GetEntry() != NPC_IMP)
            return;

        MultiImpData* data = player->CustomData.GetDefault<MultiImpData>(DATA_KEY);
        data->wanted = true;
        data->elapsed = 0;

        // re-cast = fresh set, no stacking
        DespawnExtras(player);
        SummonExtras(player, sCfg.extraCount);
    }
};

class MultiImpPlayerScript : public PlayerScript
{
public:
    MultiImpPlayerScript() : PlayerScript("MultiImpPlayerScript", { PLAYERHOOK_ON_UPDATE }) { }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!sCfg.enable)
            return;

        MultiImpData* data = player->CustomData.Get<MultiImpData>(DATA_KEY);
        if (!data || !data->wanted)
            return;

        data->elapsed += diff;
        if (data->elapsed < sCfg.syncIntervalMs)
            return;

        data->elapsed = 0;
        TickPlayer(player, data);
    }
};

void AddMultiImpScripts()
{
    new MultiImpWorldScript();
    new MultiImpSpellScript();
    new MultiImpPlayerScript();
}
