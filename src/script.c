#include "global.h"
#include "item.h"
#include "field_effect_helpers.h"
#include "field_specials.h"
#include "event_object_movement.h"
#include "item.h"
#include "overworld.h"
#include "pokedex.h"
#include "random.h"
#include "script.h"
#include "sound.h"
#include "strings.h"
#include "string_util.h"
#include "event_data.h"
#include "party_menu.h"
#include "pokemon_storage_system.h"
#include "quest_log.h"
#include "constants/songs.h"
#include "script_pokemon_util.h"
#include "constants/items.h"
#include "constants/map_scripts.h"
#include "constants/moves.h"
#include "constants/region_map_sections.h"
#include "mystery_gift.h"
#include "constants/maps.h"
#include "constants/map_scripts.h"

extern void ResetContextNpcTextColor(void); // field_specials
extern u16 CalcCRC16WithTable(u8 *data, int length); // util

#define RAM_SCRIPT_MAGIC 51

enum {
    SCRIPT_MODE_STOPPED,
    SCRIPT_MODE_BYTECODE,
    SCRIPT_MODE_NATIVE,
};

enum {
    CONTEXT_RUNNING,
    CONTEXT_WAITING,
    CONTEXT_SHUTDOWN,
};

EWRAM_DATA u8 gWalkAwayFromSignInhibitTimer = 0;
EWRAM_DATA const u8 *gRamScriptRetAddr = NULL;

static u8 sGlobalScriptContextStatus;
static u32 sUnusedVariable1;
static struct ScriptContext sGlobalScriptContext;
static u32 sUnusedVariable2;
static struct ScriptContext sImmediateScriptContext;
static bool8 sLockFieldControls;
static u8 sMsgBoxWalkawayDisabled;
static u8 sMsgBoxIsCancelable;
static u8 sQuestLogInput;
static u8 sQuestLogInputIsDpad;
static u8 sMsgIsSignpost;

extern ScrCmdFunc gScriptCmdTable[];
extern ScrCmdFunc gScriptCmdTableEnd[];
extern void *gNullScriptPtr;

void InitScriptContext(struct ScriptContext *ctx, void *cmdTable, void *cmdTableEnd)
{
    s32 i;

    ctx->mode = SCRIPT_MODE_STOPPED;
    ctx->scriptPtr = NULL;
    ctx->stackDepth = 0;
    ctx->nativePtr = NULL;
    ctx->cmdTable = cmdTable;
    ctx->cmdTableEnd = cmdTableEnd;

    for (i = 0; i < (int)ARRAY_COUNT(ctx->data); i++)
        ctx->data[i] = 0;

    for (i = 0; i < (int)ARRAY_COUNT(ctx->stack); i++)
        ctx->stack[i] = 0;
}

u8 SetupBytecodeScript(struct ScriptContext *ctx, const u8 *ptr)
{
    ctx->scriptPtr = ptr;
    ctx->mode = SCRIPT_MODE_BYTECODE;
    return 1;
}

void SetupNativeScript(struct ScriptContext *ctx, bool8 (*ptr)(void))
{
    ctx->mode = SCRIPT_MODE_NATIVE;
    ctx->nativePtr = ptr;
}

void StopScript(struct ScriptContext *ctx)
{
    ctx->mode = SCRIPT_MODE_STOPPED;
    ctx->scriptPtr = NULL;
}

bool8 RunScriptCommand(struct ScriptContext *ctx)
{
    // FRLG disabled this check, where-as it is present
    // in Ruby/Sapphire and Emerald. Why did the programmers
    // bother to remove a redundant check when it still
    // exists in Emerald?
    //if (ctx->mode == SCRIPT_MODE_STOPPED)
    //    return FALSE;

    switch (ctx->mode)
    {
    case SCRIPT_MODE_STOPPED:
        return FALSE;
    case SCRIPT_MODE_NATIVE:
        // Try to call a function in C
        // Continue to bytecode if no function or it returns TRUE
        if (ctx->nativePtr)
        {
            if (ctx->nativePtr() == TRUE)
                ctx->mode = SCRIPT_MODE_BYTECODE;
            return TRUE;
        }
        ctx->mode = SCRIPT_MODE_BYTECODE;
        // fallthrough
    case SCRIPT_MODE_BYTECODE:
        while (1)
        {
            u8 cmdCode;
            ScrCmdFunc *cmdFunc;

            if (ctx->scriptPtr == NULL)
            {
                ctx->mode = SCRIPT_MODE_STOPPED;
                return FALSE;
            }

            if (ctx->scriptPtr == gNullScriptPtr)
            {
                while (1)
                    asm("svc 2"); // HALT
            }

            cmdCode = *(ctx->scriptPtr);
            ctx->scriptPtr++;
            cmdFunc = &ctx->cmdTable[cmdCode];

            if (cmdFunc >= ctx->cmdTableEnd)
            {
                ctx->mode = SCRIPT_MODE_STOPPED;
                return FALSE;
            }

            if ((*cmdFunc)(ctx) == TRUE)
                return TRUE;
        }
    }

    return TRUE;
}

static u8 ScriptPush(struct ScriptContext *ctx, const u8 *ptr)
{
    if (ctx->stackDepth + 1 >= (int)ARRAY_COUNT(ctx->stack))
    {
        return 1;
    }
    else
    {
        ctx->stack[ctx->stackDepth] = ptr;
        ctx->stackDepth++;
        return 0;
    }
}

static const u8 *ScriptPop(struct ScriptContext *ctx)
{
    if (ctx->stackDepth == 0)
        return NULL;

    ctx->stackDepth--;
    return ctx->stack[ctx->stackDepth];
}

void ScriptJump(struct ScriptContext *ctx, const u8 *ptr)
{
    ctx->scriptPtr = ptr;
}

void ScriptCall(struct ScriptContext *ctx, const u8 *ptr)
{
    ScriptPush(ctx, ctx->scriptPtr);
    ctx->scriptPtr = ptr;
}

void ScriptReturn(struct ScriptContext *ctx)
{
    ctx->scriptPtr = ScriptPop(ctx);
}

u16 ScriptReadHalfword(struct ScriptContext *ctx)
{
    u16 value = *(ctx->scriptPtr++);
    value |= *(ctx->scriptPtr++) << 8;
    return value;
}

u32 ScriptReadWord(struct ScriptContext *ctx)
{
    u32 value0 = *(ctx->scriptPtr++);
    u32 value1 = *(ctx->scriptPtr++);
    u32 value2 = *(ctx->scriptPtr++);
    u32 value3 = *(ctx->scriptPtr++);
    return (((((value3 << 8) + value2) << 8) + value1) << 8) + value0;
}

void LockPlayerFieldControls(void)
{
    sLockFieldControls = TRUE;
}

void UnlockPlayerFieldControls(void)
{
    sLockFieldControls = FALSE;
}

bool8 ArePlayerFieldControlsLocked(void)
{
    return sLockFieldControls;
}

void SetQuestLogInputIsDpadFlag(void)
{
    sQuestLogInputIsDpad = TRUE;
}

void ClearQuestLogInputIsDpadFlag(void)
{
    sQuestLogInputIsDpad = FALSE;
}

bool8 IsQuestLogInputDpad(void)
{
    if(sQuestLogInputIsDpad == TRUE)
        return TRUE;
    else
        return FALSE;
}

void RegisterQuestLogInput(u8 var)
{
    sQuestLogInput = var;
}

void ClearQuestLogInput(void)
{
    sQuestLogInput = 0;
}

u8 GetRegisteredQuestLogInput(void)
{
    return sQuestLogInput;
}

void DisableMsgBoxWalkaway(void)
{
    sMsgBoxWalkawayDisabled = TRUE;
}

void EnableMsgBoxWalkaway(void)
{
    sMsgBoxWalkawayDisabled = FALSE;
}

bool8 IsMsgBoxWalkawayDisabled(void)
{
    return sMsgBoxWalkawayDisabled;
}

void SetWalkingIntoSignVars(void)
{
    gWalkAwayFromSignInhibitTimer = 6;
    sMsgBoxIsCancelable = TRUE;
}

void ClearMsgBoxCancelableState(void)
{
    sMsgBoxIsCancelable = FALSE;
}

bool8 CanWalkAwayToCancelMsgBox(void)
{
    if(sMsgBoxIsCancelable == TRUE)
        return TRUE;
    else
        return FALSE;
}

void MsgSetSignpost(void)
{
    sMsgIsSignpost = TRUE;
}

void MsgSetNotSignpost(void)
{
    sMsgIsSignpost = FALSE;
}

bool8 IsMsgSignpost(void)
{
    if(sMsgIsSignpost == TRUE)
        return TRUE;
    else
        return FALSE;
}

void ResetFacingNpcOrSignpostVars(void)
{
    ResetContextNpcTextColor();
    MsgSetNotSignpost();
}

// The ScriptContext_* functions work with the primary script context,
// which yields control back to native code should the script make a wait call.

// Checks if the global script context is able to be run right now.
bool8 ScriptContext_IsEnabled(void)
{
    if (sGlobalScriptContextStatus == CONTEXT_RUNNING)
        return TRUE;
    else
        return FALSE;
}

// Re-initializes the global script context to zero.
void ScriptContext_Init(void)
{
    InitScriptContext(&sGlobalScriptContext, gScriptCmdTable, gScriptCmdTableEnd);
    sGlobalScriptContextStatus = CONTEXT_SHUTDOWN;
}

// Runs the script until the script makes a wait* call, then returns true if 
// there's more script to run, or false if the script has hit the end. 
// This function also returns false if the context is finished 
// or waiting (after a call to _Stop)
bool8 ScriptContext_RunScript(void)
{
    if (sGlobalScriptContextStatus == CONTEXT_SHUTDOWN)
        return FALSE;

    if (sGlobalScriptContextStatus == CONTEXT_WAITING)
        return FALSE;

    LockPlayerFieldControls();

    if (!RunScriptCommand(&sGlobalScriptContext))
    {
        sGlobalScriptContextStatus = CONTEXT_SHUTDOWN;
        UnlockPlayerFieldControls();
        return FALSE;
    }

    return TRUE;
}

// Sets up a new script in the global context and enables the context
void ScriptContext_SetupScript(const u8 *ptr)
{
    ClearMsgBoxCancelableState();
    EnableMsgBoxWalkaway();
    ClearQuestLogInputIsDpadFlag();

    InitScriptContext(&sGlobalScriptContext, gScriptCmdTable, gScriptCmdTableEnd);
    SetupBytecodeScript(&sGlobalScriptContext, ptr);
    LockPlayerFieldControls();
    sGlobalScriptContextStatus = CONTEXT_RUNNING;
}

// Puts the script into waiting mode; usually called from a wait* script command.
void ScriptContext_Stop(void)
{
    sGlobalScriptContextStatus = CONTEXT_WAITING;
}

// Puts the script into running mode.
void ScriptContext_Enable(void)
{
    sGlobalScriptContextStatus = CONTEXT_RUNNING;
    LockPlayerFieldControls();
}

// Sets up and runs a script in its own context immediately. The script will be
// finished when this function returns. Used mainly by all of the map header
// scripts (except the frame table scripts).
void RunScriptImmediately(const u8 *ptr)
{
    InitScriptContext(&sImmediateScriptContext, &gScriptCmdTable, &gScriptCmdTableEnd);
    SetupBytecodeScript(&sImmediateScriptContext, ptr);
    while (RunScriptCommand(&sImmediateScriptContext) == TRUE);
}

static u8 *MapHeaderGetScriptTable(u8 tag)
{
    const u8 *mapScripts = gMapHeader.mapScripts;

    if (mapScripts == NULL)
        return NULL;

    while (1)
    {
        if (*mapScripts == 0)
            return NULL;
        if (*mapScripts == tag)
        {
            mapScripts++;
            return T2_READ_PTR(mapScripts);
        }
        mapScripts += 5;
    }
}

static void MapHeaderRunScriptType(u8 tag)
{
    u8 *ptr = MapHeaderGetScriptTable(tag);
    if (ptr != NULL)
        RunScriptImmediately(ptr);
}

static u8 *MapHeaderCheckScriptTable(u8 tag)
{
    u8 *ptr = MapHeaderGetScriptTable(tag);

    if (ptr == NULL)
        return NULL;

    while (1)
    {
        u16 varIndex1;
        u16 varIndex2;

        // Read first var (or .2byte terminal value)
        varIndex1 = T1_READ_16(ptr);
        if (!varIndex1)
            return NULL; // Reached end of table
        ptr += 2;

        // Read second var
        varIndex2 = T1_READ_16(ptr);
        ptr += 2;

        // Run map script if vars are equal
        if (VarGet(varIndex1) == VarGet(varIndex2))
            return T2_READ_PTR(ptr);
        ptr += 4;
    }
}

void RunOnLoadMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_LOAD);
}

void RunOnTransitionMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_TRANSITION);
}

void RunOnResumeMapScript(void)
{
    FlagClear(FLAG_SYS_ON_RESUME);
    MapHeaderRunScriptType(MAP_SCRIPT_ON_RESUME);
}

void RunOnReturnToFieldMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_RETURN_TO_FIELD);
}

void RunOnDiveWarpMapScript(void)
{
    MapHeaderRunScriptType(MAP_SCRIPT_ON_DIVE_WARP);
}

bool8 TryRunOnFrameMapScript(void)
{
    u8 *ptr;

    if (gQuestLogState == QL_STATE_PLAYBACK_LAST)
        return FALSE;

    ptr = MapHeaderCheckScriptTable(MAP_SCRIPT_ON_FRAME_TABLE);

    if (!ptr)
        return FALSE;

    ScriptContext_SetupScript(ptr);
    return TRUE;
}

void TryRunOnWarpIntoMapScript(void)
{
    u8 *ptr = MapHeaderCheckScriptTable(MAP_SCRIPT_ON_WARP_INTO_MAP_TABLE);
    if (ptr)
        RunScriptImmediately(ptr);
}

u32 CalculateRamScriptChecksum(void)
{
    return CalcCRC16WithTable((u8 *)(&gSaveBlock1Ptr->ramScript.data), sizeof(gSaveBlock1Ptr->ramScript.data));
}

void ClearRamScript(void)
{
    CpuFill32(0, &gSaveBlock1Ptr->ramScript, sizeof(struct RamScript));
}

bool8 InitRamScript(u8 *script, u16 scriptSize, u8 mapGroup, u8 mapNum, u8 objectId)
{
    struct RamScriptData *scriptData = &gSaveBlock1Ptr->ramScript.data;

    ClearRamScript();

    if (scriptSize > sizeof(scriptData->script))
        return FALSE;

    scriptData->magic = RAM_SCRIPT_MAGIC;
    scriptData->mapGroup = mapGroup;
    scriptData->mapNum = mapNum;
    scriptData->objectId = objectId;
    memcpy(scriptData->script, script, scriptSize);
    gSaveBlock1Ptr->ramScript.checksum = CalculateRamScriptChecksum();
    return TRUE;
}

const u8 *GetRamScript(u8 objectId, const u8 *script)
{
    struct RamScriptData *scriptData = &gSaveBlock1Ptr->ramScript.data;
    gRamScriptRetAddr = NULL;
    if (scriptData->magic != RAM_SCRIPT_MAGIC)
        return script;
    if (scriptData->mapGroup != gSaveBlock1Ptr->location.mapGroup)
        return script;
    if (scriptData->mapNum != gSaveBlock1Ptr->location.mapNum)
        return script;
    if (scriptData->objectId != objectId)
        return script;
    if (CalculateRamScriptChecksum() != gSaveBlock1Ptr->ramScript.checksum)
    {
        ClearRamScript();
        return script;
    }
    else
    {
        gRamScriptRetAddr = script;
        return scriptData->script;
    }
}

bool32 ValidateRamScript(void)
{
    struct RamScriptData *scriptData = &gSaveBlock1Ptr->ramScript.data;
    if (scriptData->magic != RAM_SCRIPT_MAGIC)
        return FALSE;
    if (scriptData->mapGroup != MAP_GROUP(UNDEFINED))
        return FALSE;
    if (scriptData->mapNum != MAP_NUM(UNDEFINED))
        return FALSE;
    if (scriptData->objectId != 0xFF)
        return FALSE;
    if (CalculateRamScriptChecksum() != gSaveBlock1Ptr->ramScript.checksum)
        return FALSE;
    return TRUE;
}

u8 *GetSavedRamScriptIfValid(void)
{
    struct RamScriptData *scriptData = &gSaveBlock1Ptr->ramScript.data;
    if (!ValidateSavedWonderCard())
        return NULL;
    if (scriptData->magic != RAM_SCRIPT_MAGIC)
        return NULL;
    if (scriptData->mapGroup != MAP_GROUP(UNDEFINED))
        return NULL;
    if (scriptData->mapNum != MAP_NUM(UNDEFINED))
        return NULL;
    if (scriptData->objectId != 0xFF)
        return NULL;
    if (CalculateRamScriptChecksum() != gSaveBlock1Ptr->ramScript.checksum)
    {
        ClearRamScript();
        return NULL;
    }
    else
    {
        return scriptData->script;
    }
}

void InitRamScript_NoObjectEvent(u8 *script, u16 scriptSize)
{
    if (scriptSize > sizeof(gSaveBlock1Ptr->ramScript.data.script))
        scriptSize = sizeof(gSaveBlock1Ptr->ramScript.data.script);
    InitRamScript(script, scriptSize, MAP_GROUP(UNDEFINED), MAP_NUM(UNDEFINED), 0xFF);
}

void GetStartingLevelOfRoute5DaycareMon(void)
{
    ConvertIntToDecimalStringN(gStringVar3, GetLevelFromBoxMonExp(&gSaveBlock1Ptr->route5DayCareMon.mon), STR_CONV_MODE_LEFT_ALIGN, 2);
}

void CheckCurrentMasterTitle(void)
{
    gSpecialVar_Result = gSaveBlock1Ptr->masterTrainerTitle;
}

void SetCurrentMasterTitle(void)
{
    gSaveBlock1Ptr->masterTrainerTitle = gSpecialVar_Result;
}

void CheckHasAnyMasterTitle(void)
{
    u32 i;
    bool8 flag = TRUE;

    if(gSaveBlock1Ptr->masterTrainerTitle != 0)
    {
        gSpecialVar_Result = TRUE;
        return;
    }

    for(i = 1; i < 152; i++) //flags start at 1, which is SPECIES_BULBASAUR
    {
        flag = CheckMasterTrainerFlag(i);
        if(flag)
        {
            gSpecialVar_Result = TRUE;
            return;
        }
    }
    gSpecialVar_Result = FALSE;
}

void CheckAllMasterTrainerFlags(void)
{
    u32 i;
    bool8 flag = TRUE;

    for(i = 1; i < 152; i++) //flags start at 1, which is SPECIES_BULBASAUR
    {
        flag = CheckMasterTrainerFlag(i);
        if(!flag)
        {
            gSpecialVar_Result = FALSE;
            return;
        }
    }
    gSpecialVar_Result = TRUE;
}

void ShouldChangeMasterTrainerMovementType(void)
{
    gSpecialVar_Result = CheckMasterTrainerFlag(gSpecialVar_0x8009);
}

void ClearAllButFirstPokemon(void)
{
    u32 i;
    for (i = 1; i < PARTY_SIZE; i++)
        ZeroMonData(&gPlayerParty[i]);
}

void CheckSlot1EVTotal(void)
{
    u8 evs[NUM_STATS];
    u16 totalEVs = 0;
    u32 i;

    for (i = 0; i < NUM_STATS; i++)
    {
        evs[i] = GetMonData(&gPlayerParty[0], MON_DATA_HP_EV + i, NULL);
        totalEVs += evs[i];
    }

    if(totalEVs >= 100)
        gSpecialVar_Result = TRUE;
    else
        gSpecialVar_Result = FALSE;
}

void CheckMasterPokemonInSlot1(void)
{
    if(GetMonData(&gPlayerParty[0], MON_DATA_SPECIES_OR_EGG, NULL) == gSpecialVar_0x8009)
        gSpecialVar_Result = TRUE;
    else
        gSpecialVar_Result = FALSE;

    //loading a or an into STR_VAR_3 for certain Pokemon...
    switch(gSpecialVar_0x8009)
    {
        case SPECIES_IVYSAUR:
        case SPECIES_ODDISH:
        case SPECIES_ARCANINE:
        case SPECIES_EXEGGCUTE:
        case SPECIES_EXEGGUTOR:
        case SPECIES_EEVEE:
            StringCopy(gStringVar3, gText_An);
            break;
        default:
            StringCopy(gStringVar3, gText_A);
            break;
    }
}

void ShouldSpawnSoftlockClerk(void)
{
    gSpecialVar_Result = CheckAssetsForSoftlock();
}

void HideWarpArrowSprite(void)
{
    struct ObjectEvent *playerObjEvent = &gObjectEvents[gPlayerAvatar.objectEventId];
    SetSpriteInvisible(playerObjEvent->warpArrowSpriteId);
}

void CoordsOfPMCExitWarpTo80068007(void)
{
    switch(gMapHeader.regionMapSectionId)
    {
        case MAPSEC_INDIGO_PLATEAU:
            gSpecialVar_0x8006 = 11;
            gSpecialVar_0x8007 = 16;
            return;
        case MAPSEC_ONE_ISLAND:
            gSpecialVar_0x8006 = 9;
            gSpecialVar_0x8007 = 9;
            return;
        default:
            gSpecialVar_0x8006 = 7;
            gSpecialVar_0x8007 = 8;
            return;
    }
}

void HasNationalMonToVarResult(void)
{
    gSpecialVar_Result = HasNationalMon();
}

void TeachTrappedTentacoolSurf(void)
{
    u32 i;
    u32 move = MOVE_SURF;
    u32 pp = 15;
    if(gSpecialVar_0x8007 == 0) //party
    {
        i = CalculatePlayerPartyCount() - 1;
        SetMonData(&gPlayerParty[i], MON_DATA_MOVE4, &move);
        SetMonData(&gPlayerParty[i], MON_DATA_PP1 + 3, &pp);
        return;
    }
    else //box
    {
        SetBoxMonDataAt(gSpecialVar_MonBoxId, gSpecialVar_MonBoxPos, MON_DATA_MOVE4, &move);
        SetBoxMonDataAt(gSpecialVar_MonBoxId, gSpecialVar_MonBoxPos, MON_DATA_PP1 + 3, &pp);
        return;
    }
}

void CheckPlayerTrappedOnCinnabar(void)
{
    bool32 hasHM03 = CheckBagHasItem(ITEM_HM03, 1);
    u32 i, j;

    for (i = 0; i < PARTY_SIZE; i++) {
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES, NULL) == SPECIES_NONE) {
            break;
        } else {
            struct Pokemon* partyMon = &gPlayerParty[i];
            if (!GetMonData(partyMon, MON_DATA_IS_EGG) && MonKnowsMove(partyMon, MOVE_SURF)) {
                gSpecialVar_Result = 0;
                return;
            }
            if (hasHM03 && CanMonLearnTMHM(partyMon, ITEM_HM03 - ITEM_TM01_FOCUS_PUNCH)) {
                gSpecialVar_Result = 0;
                return;
            }
        }
    }

    for (i = 0; i < TOTAL_BOXES_COUNT; i++) {
        for (j = 0; j < IN_BOX_COUNT; j++) {
            if (GetBoxMonDataAt(i, j, MON_DATA_SPECIES) == SPECIES_NONE) {
                continue;
            } else {
                struct Pokemon tempMon;
                BoxMonToMon(GetBoxedMonPtr(i, j), &tempMon);
                if (!GetMonData(&tempMon, MON_DATA_IS_EGG) && MonKnowsMove(&tempMon, MOVE_SURF)) {
                    gSpecialVar_Result = 0;
                    return;
                }
                if (hasHM03 && CanMonLearnTMHM(&tempMon, ITEM_HM03 - ITEM_TM01_FOCUS_PUNCH)) {
                    gSpecialVar_Result = 0;
                    return;
                }
            }
        }
    }

    if(hasHM03)
        gSpecialVar_Result = 1;
    else
        gSpecialVar_Result = 2; // need to teach Tentacool Surf
}

void CheckNuzlockeMode(void)
{
    gSpecialVar_Result = gSaveBlock1Ptr->keyFlags.nuzlocke;
}

void CheckNoFreeHealsMode(void)
{
    gSpecialVar_Result = gSaveBlock1Ptr->keyFlags.noPMC;
}

#define SET_SPEAROW_STATE 0
#define SET_TOLD_FAMECHECKER 1
#define SET_VISITOR_STATE 2
#define SET_LEVEL_GROWTH 3
#define CHECK_SPEAROW_STATE 4
#define CHECK_TOLD_FAMECHECKER 5
#define CHECK_VISITOR_STATE 6
#define CHECK_LEVEL_GROWTH 7
#define SET_BOXES_MOVED 8
#define CHECK_BOXES_MOVED 9
#define CHECK_ANY_VISITORS 10
#define CHECK_ALL_TOLD 11
#define CREATE_VISITOR_STRING 12

static bool8 AllPossibleGymLeadersPresent(void)
{
    struct BattleHouse* BattleHouseVar = &gSaveBlock1Ptr->battleHouseData; 
    if(BattleHouseVar->toldBrock && !FlagGet(FLAG_BATTLEHOUSE_BROCK_VISITOR))
    {
        return FALSE;
    }
    if(BattleHouseVar->toldMisty && !FlagGet(FLAG_BATTLEHOUSE_MISTY_VISITOR))
    {
        return FALSE;
    }
    if(BattleHouseVar->toldLtSurge && !FlagGet(FLAG_BATTLEHOUSE_LT_SURGE_VISITOR))
    {
        return FALSE;
    }
    if(BattleHouseVar->toldErika && !FlagGet(FLAG_BATTLEHOUSE_ERIKA_VISITOR))
    {
        return FALSE;
    }
    if(BattleHouseVar->toldKoga && !FlagGet(FLAG_BATTLEHOUSE_KOGA_VISITOR))
    {
        return FALSE;
    }
    if(BattleHouseVar->toldSabrina && !FlagGet(FLAG_BATTLEHOUSE_SABRINA_VISITOR))
    {
        return FALSE;
    }
    if(BattleHouseVar->toldBlaine && !FlagGet(FLAG_BATTLEHOUSE_BLAINE_VISITOR))
    {
        return FALSE;
    }
    return TRUE;
}

u8 ReturnBattleHouseLevel(void)
{
    struct BattleHouse* BattleHouseVar = &gSaveBlock1Ptr->battleHouseData;
    u8 levelGrowth = BattleHouseVar->levelGrowth;
    return levelGrowth;
}

void BattleHouseScaleLevelUp(void)
{
    if(gSaveBlock1Ptr->battleHouseData.levelGrowth < 12)
        gSaveBlock1Ptr->battleHouseData.levelGrowth++;
}

void UpdateBattleHouseStepCounter(void)
{
    struct BattleHouse* BattleHouseVar = &gSaveBlock1Ptr->battleHouseData;
    u8 chanceOfVisit = 46;

    if((BattleHouseVar->boxesMoved || BattleHouseVar->spearowState
        || BattleHouseVar->toldBrock || BattleHouseVar->toldMisty || BattleHouseVar->toldLtSurge
        || BattleHouseVar->toldErika || BattleHouseVar->toldKoga || BattleHouseVar->toldSabrina || BattleHouseVar->toldBlaine) 
        && gMapHeader.regionMapSectionId != MAPSEC_SEVEN_ISLAND)
    {
        if(BattleHouseVar->steps != 255)
            BattleHouseVar->steps++;
        else
        {
            BattleHouseVar->steps++;
            if(BattleHouseVar->spearowState && !BattleHouseVar->boxesMoved)
            {   //Spearow left for the first time, bring it back, open basement, force first leader visit
                BattleHouseVar->spearowState = 0;
                BattleHouseVar->boxesMoved = 1;
                if(BattleHouseVar->toldBrock)
                    FlagSet(FLAG_BATTLEHOUSE_BROCK_VISITOR);
                if(BattleHouseVar->toldMisty)
                    FlagSet(FLAG_BATTLEHOUSE_MISTY_VISITOR);
                if(BattleHouseVar->toldLtSurge)
                    FlagSet(FLAG_BATTLEHOUSE_LT_SURGE_VISITOR);
                if(BattleHouseVar->toldErika)
                    FlagSet(FLAG_BATTLEHOUSE_ERIKA_VISITOR);
                if(BattleHouseVar->toldKoga)
                    FlagSet(FLAG_BATTLEHOUSE_KOGA_VISITOR);
                if(BattleHouseVar->toldSabrina)
                    FlagSet(FLAG_BATTLEHOUSE_SABRINA_VISITOR);
                if(BattleHouseVar->toldBlaine)
                    FlagSet(FLAG_BATTLEHOUSE_BLAINE_VISITOR);
                return;
            }
            if(BattleHouseVar->spearowState)
            {   //Spearow left, check if it returns
                if(Random() % 2 == 0) //50% chance
                {
                    BattleHouseVar->spearowState = 0;
                }
                return; //no new visitors while Spearow is out
            }
            if(!AllPossibleGymLeadersPresent() && (BattleHouseVar->spawnFails == 6 || Random() % 256 < chanceOfVisit))
            {   //Gym Leader visiting, ~18% chance; will start forcing visits after 6 misses
                u8 counter = 0;
                do{
                    u8 leader = Random() % 7;
                    switch(leader)
                    {
                        case 0:
                            if(BattleHouseVar->toldBrock && !FlagGet(FLAG_BATTLEHOUSE_BROCK_VISITOR))
                            {
                                FlagSet(FLAG_BATTLEHOUSE_BROCK_VISITOR);
                                BattleHouseVar->spawnFails = 0;
                                return;
                            }
                            break;
                        case 1:
                            if(BattleHouseVar->toldMisty && !FlagGet(FLAG_BATTLEHOUSE_MISTY_VISITOR))
                            {
                                FlagSet(FLAG_BATTLEHOUSE_MISTY_VISITOR);
                                BattleHouseVar->spawnFails = 0;
                                return;
                            }
                            break;
                        case 2:
                            if(BattleHouseVar->toldLtSurge && !FlagGet(FLAG_BATTLEHOUSE_LT_SURGE_VISITOR))
                            {
                                FlagSet(FLAG_BATTLEHOUSE_LT_SURGE_VISITOR);
                                BattleHouseVar->spawnFails = 0;
                                return;
                            }
                            break;
                        case 3:
                            if(BattleHouseVar->toldErika && !FlagGet(FLAG_BATTLEHOUSE_ERIKA_VISITOR))
                            {
                                FlagSet(FLAG_BATTLEHOUSE_ERIKA_VISITOR);
                                BattleHouseVar->spawnFails = 0;
                                return;
                            }
                            break;
                        case 4:
                            if(BattleHouseVar->toldKoga && !FlagGet(FLAG_BATTLEHOUSE_KOGA_VISITOR))
                            {
                                FlagSet(FLAG_BATTLEHOUSE_KOGA_VISITOR);
                                BattleHouseVar->spawnFails = 0;
                                return;
                            }
                            break;
                        case 5:
                            if(BattleHouseVar->toldSabrina && !FlagGet(FLAG_BATTLEHOUSE_SABRINA_VISITOR))
                            {
                                FlagSet(FLAG_BATTLEHOUSE_SABRINA_VISITOR);
                                BattleHouseVar->spawnFails = 0;
                                return;
                            }
                            break;
                        case 6:
                            if(BattleHouseVar->toldBlaine && !FlagGet(FLAG_BATTLEHOUSE_BLAINE_VISITOR))
                            {
                                FlagSet(FLAG_BATTLEHOUSE_BLAINE_VISITOR);
                                BattleHouseVar->spawnFails = 0;
                                return;
                            }
                            break;
                    }
                    counter++;
                }while(counter < 3 || BattleHouseVar->spawnFails >= 6); //rerolls up to 3 times if doesn't hit; forces a spawn if failed 6 times
                if(counter == 3) //failed 3 rerolls, increment spawnFails
                    if(BattleHouseVar->spawnFails != 7 && !AllPossibleGymLeadersPresent())
                        BattleHouseVar->spawnFails++;
            }
            else
            {
                if(BattleHouseVar->spawnFails != 7 && !AllPossibleGymLeadersPresent())
                    BattleHouseVar->spawnFails++;
            }
        }
    }
}

// can be removed, probably
void CheckVisitorState(void)
{
    u8 argument = gSpecialVar_0x8004;
    struct BattleHouse* BattleHouseVar = &gSaveBlock1Ptr->battleHouseData;
    switch(argument)
    {
        case FAMECHECKER_BROCK:
            VarSet(VAR_ELEVATOR_FLOOR, 0);
            break;
        case FAMECHECKER_MISTY:
                FlagSet(FLAG_TEMP_2);
            break;
        case FAMECHECKER_LTSURGE:
            VarSet(VAR_ELEVATOR_FLOOR, 0);
        case FAMECHECKER_ERIKA:
            gSpecialVar_0x8009 = FlagGet(FLAG_BATTLEHOUSE_ERIKA_VISITOR);
            /*if (FlagGet(FLAG_BATTLEHOUSE_ERIKA_VISITOR) == 1)
                FlagSet(FLAG_TEMP_4);
            else
                FlagClear(FLAG_TEMP_4);*/
            break;
        case FAMECHECKER_KOGA:
            VarSet(VAR_ELEVATOR_FLOOR, 0);
            /*if (FlagGet(FLAG_BATTLEHOUSE_KOGA_VISITOR))
                FlagSet(FLAG_TEMP_5);
            else
                FlagClear(FLAG_TEMP_5);*/
            break;
        case FAMECHECKER_SABRINA:
            VarSet(VAR_ELEVATOR_FLOOR, 0);
            /*if (FlagGet(FLAG_BATTLEHOUSE_SABRINA_VISITOR))
                FlagSet(FLAG_TEMP_6);
            else
                FlagClear(FLAG_TEMP_6);*/
            break;
        case FAMECHECKER_BLAINE:
            gSpecialVar_0x8009 = 0;
            /*if (FlagGet(FLAG_BATTLEHOUSE_BLAINE_VISITOR))
                FlagSet(FLAG_TEMP_7);
            else
                FlagClear(FLAG_TEMP_7);*/
            break;
    }
}

void UseBattleHouseVar(void)
{
    u8 type = gSpecialVar_0x8003;
    u8 argument = gSpecialVar_0x8004;
    struct BattleHouse* BattleHouseVar = &gSaveBlock1Ptr->battleHouseData;
    u8 totalCount = 0;
    u8 runningCount = 0;
    switch(type)
    {
        case SET_SPEAROW_STATE:
            BattleHouseVar->spearowState ^= 1; //toggle state
            break;
        case SET_TOLD_FAMECHECKER:
            switch(argument)
            {
                case FAMECHECKER_BROCK:
                    BattleHouseVar->toldBrock = 1;
                    break;
                case FAMECHECKER_MISTY:
                    BattleHouseVar->toldMisty = 1;
                    break;
                case FAMECHECKER_LTSURGE:
                    BattleHouseVar->toldLtSurge = 1;
                    break;
                case FAMECHECKER_ERIKA:
                    BattleHouseVar->toldErika = 1;
                    break;
                case FAMECHECKER_KOGA:
                    BattleHouseVar->toldKoga = 1;
                    break;
                case FAMECHECKER_SABRINA:
                    BattleHouseVar->toldSabrina = 1;
                    break;
                case FAMECHECKER_BLAINE:
                    BattleHouseVar->toldBlaine = 1;
                    break;
            }
            break;
        case CHECK_SPEAROW_STATE:
            gSpecialVar_Result = BattleHouseVar->spearowState;
            break;
        case CHECK_TOLD_FAMECHECKER:
            switch(argument)
            {
                case FAMECHECKER_BROCK:
                    gSpecialVar_Result = BattleHouseVar->toldBrock;
                    break;
                case FAMECHECKER_MISTY:
                    gSpecialVar_Result = BattleHouseVar->toldMisty;
                    break;
                case FAMECHECKER_LTSURGE:
                    gSpecialVar_Result = BattleHouseVar->toldLtSurge;
                    break;
                case FAMECHECKER_ERIKA:
                    gSpecialVar_Result = BattleHouseVar->toldErika;
                    break;
                case FAMECHECKER_KOGA:
                    gSpecialVar_Result = BattleHouseVar->toldKoga;
                    break;
                case FAMECHECKER_SABRINA:
                    gSpecialVar_Result = BattleHouseVar->toldSabrina;
                    break;
                case FAMECHECKER_BLAINE:
                    gSpecialVar_Result = BattleHouseVar->toldBlaine;
                    break;
            }
            break;
        case CHECK_LEVEL_GROWTH:
            if(BattleHouseVar->levelGrowth > 12)
                gSpecialVar_Result = 12;
            else 
                gSpecialVar_Result = BattleHouseVar->levelGrowth;
            break;
        case SET_BOXES_MOVED:
            BattleHouseVar->boxesMoved = 1;
            break;
        case CHECK_BOXES_MOVED:
            gSpecialVar_Result = BattleHouseVar->boxesMoved;
            break;
        case CHECK_ANY_VISITORS:
            gSpecialVar_Result = 0;
            if(FlagGet(FLAG_BATTLEHOUSE_BROCK_VISITOR))
            {
                gSpecialVar_Result++;
                StringCopy(gStringVar1, gText_Brock);
            }
            if(FlagGet(FLAG_BATTLEHOUSE_MISTY_VISITOR))
            {
                gSpecialVar_Result++;
                StringCopy(gStringVar1, gText_Misty);
            }
            if(FlagGet(FLAG_BATTLEHOUSE_LT_SURGE_VISITOR))
            {
                gSpecialVar_Result++;
                StringCopy(gStringVar1, gText_LtSurge);
            }
            if(FlagGet(FLAG_BATTLEHOUSE_ERIKA_VISITOR))
            {
                gSpecialVar_Result++;
                StringCopy(gStringVar1, gText_Erika);
            }
            if(FlagGet(FLAG_BATTLEHOUSE_KOGA_VISITOR))
            {
                gSpecialVar_Result++;
                StringCopy(gStringVar1, gText_Koga);
            }
            if(FlagGet(FLAG_BATTLEHOUSE_SABRINA_VISITOR))
            {
                gSpecialVar_Result++;
                StringCopy(gStringVar1, gText_Sabrina);
            }
            if(FlagGet(FLAG_BATTLEHOUSE_BLAINE_VISITOR))
            {
                gSpecialVar_Result++;
                StringCopy(gStringVar1, gText_Blaine);
            }
            break;
        case CHECK_ALL_TOLD:
            gSpecialVar_Result = 0;
            if(BattleHouseVar->toldBrock)
                gSpecialVar_Result++;
            if(BattleHouseVar->toldMisty)
                gSpecialVar_Result++;
            if(BattleHouseVar->toldLtSurge)
                gSpecialVar_Result++;
            if(BattleHouseVar->toldErika)
                gSpecialVar_Result++;
            if(BattleHouseVar->toldKoga)
                gSpecialVar_Result++;
            if(BattleHouseVar->toldSabrina)
                gSpecialVar_Result++;
            if(BattleHouseVar->toldBlaine)
                gSpecialVar_Result++;
            break;
        case CREATE_VISITOR_STRING: //used for mailbox outside
            if(FlagGet(FLAG_BATTLEHOUSE_BROCK_VISITOR))
                totalCount++;
            if(FlagGet(FLAG_BATTLEHOUSE_MISTY_VISITOR))
                totalCount++;
            if(FlagGet(FLAG_BATTLEHOUSE_LT_SURGE_VISITOR))
                totalCount++;
            if(FlagGet(FLAG_BATTLEHOUSE_ERIKA_VISITOR))
                totalCount++;
            if(FlagGet(FLAG_BATTLEHOUSE_KOGA_VISITOR))
                totalCount++;
            if(FlagGet(FLAG_BATTLEHOUSE_SABRINA_VISITOR))
                totalCount++;
            if(FlagGet(FLAG_BATTLEHOUSE_BLAINE_VISITOR))
                totalCount++;
            StringCopy(gStringVar1, gExpandedPlaceholder_Empty);
            gSpecialVar_Result = 0;
            if(FlagGet(FLAG_BATTLEHOUSE_BROCK_VISITOR))
            {
                gSpecialVar_Result++;
                runningCount++;
                StringAppend(gStringVar1, gText_Brock);
                if(totalCount != runningCount && totalCount != 2)
                {
                    StringAppend(gStringVar1, gText_CommaSpace);
                    if(runningCount == (totalCount - 1))
                    {
                        StringAppend(gStringVar1, gText_AndSpace);
                    }
                }
                if(totalCount == 2 && runningCount != 2)
                {
                    StringAppend(gStringVar1, gText_RegionMap_Space);
                    StringAppend(gStringVar1, gText_AndSpace);
                }
            }
            if(FlagGet(FLAG_BATTLEHOUSE_MISTY_VISITOR))
            {
                gSpecialVar_Result++;
                runningCount++;
                StringAppend(gStringVar1, gText_Misty);
                if(totalCount != runningCount && totalCount != 2)
                {
                    StringAppend(gStringVar1, gText_CommaSpace);
                    if(runningCount == (totalCount - 1))
                    {
                        StringAppend(gStringVar1, gText_AndSpace);
                    }
                }
                if(totalCount == 2 && runningCount != 2)
                {
                    StringAppend(gStringVar1, gText_RegionMap_Space);
                    StringAppend(gStringVar1, gText_AndSpace);
                }
            }
            if(FlagGet(FLAG_BATTLEHOUSE_LT_SURGE_VISITOR))
            {
                gSpecialVar_Result++;
                runningCount++;
                StringAppend(gStringVar1, gText_LtSurge);
                if(totalCount != runningCount && totalCount != 2)
                {
                    StringAppend(gStringVar1, gText_CommaSpace);
                    if(runningCount == (totalCount - 1))
                    {
                        StringAppend(gStringVar1, gText_AndSpace);
                    }
                }
                if(totalCount == 2 && runningCount != 2)
                {
                    StringAppend(gStringVar1, gText_RegionMap_Space);
                    StringAppend(gStringVar1, gText_AndSpace);
                }
            }
            if(FlagGet(FLAG_BATTLEHOUSE_ERIKA_VISITOR))
            {
                gSpecialVar_Result++;
                runningCount++;
                StringAppend(gStringVar1, gText_Erika);
                if(totalCount != runningCount && totalCount != 2)
                {
                    StringAppend(gStringVar1, gText_CommaSpace);
                    if(runningCount == (totalCount - 1))
                    {
                        StringAppend(gStringVar1, gText_AndSpace);
                    }
                }
                if(totalCount == 2 && runningCount != 2)
                {
                    StringAppend(gStringVar1, gText_RegionMap_Space);
                    StringAppend(gStringVar1, gText_AndSpace);
                }
            }
            if(FlagGet(FLAG_BATTLEHOUSE_KOGA_VISITOR))
            {
                runningCount++;
                if(gSpecialVar_Result == 4 && totalCount != runningCount)
                {
                    StringAppend(gStringVar1, gText_NewLine);
                    gSpecialVar_Result = 0;
                }
                gSpecialVar_Result++;
                StringAppend(gStringVar1, gText_Koga);
                if(totalCount != runningCount && totalCount != 2)
                {
                    StringAppend(gStringVar1, gText_CommaSpace);
                    if(runningCount == (totalCount - 1))
                    {
                        StringAppend(gStringVar1, gText_AndSpace);
                    }
                }
                if(totalCount == 2 && runningCount != 2)
                {
                    StringAppend(gStringVar1, gText_RegionMap_Space);
                    StringAppend(gStringVar1, gText_AndSpace);
                }
            }
            if(FlagGet(FLAG_BATTLEHOUSE_SABRINA_VISITOR))
            {
                runningCount++;
                if(gSpecialVar_Result == 4)
                {
                    StringAppend(gStringVar1, gText_NewLine);
                    gSpecialVar_Result = 0;
                }
                gSpecialVar_Result++;
                StringAppend(gStringVar1, gText_Sabrina);
                if(totalCount != runningCount && totalCount != 2)
                {
                    StringAppend(gStringVar1, gText_CommaSpace);
                    if(runningCount == (totalCount - 1))
                    {
                        StringAppend(gStringVar1, gText_AndSpace);
                    }
                }
                if(totalCount == 2 && runningCount != 2)
                {
                    StringAppend(gStringVar1, gText_RegionMap_Space);
                    StringAppend(gStringVar1, gText_AndSpace);
                }
            }
            if(FlagGet(FLAG_BATTLEHOUSE_BLAINE_VISITOR))
            {
                runningCount++;
                if(gSpecialVar_Result == 4)
                {
                    StringAppend(gStringVar1, gText_NewLine);
                    gSpecialVar_Result = 0;
                }
                gSpecialVar_Result++;
                StringAppend(gStringVar1, gText_Blaine);
            }
            break;

    }
}

#undef SET_SPEAROW_STATE
#undef SET_TOLD_FAMECHECKER
#undef SET_VISITOR_STATE
#undef SET_LEVEL_GROWTH
#undef CHECK_SPEAROW_STATE
#undef CHECK_TOLD_FAMECHECKER
#undef CHECK_VISITOR_STATE
#undef CHECK_LEVEL_GROWTH
#undef SET_BOXES_MOVED
#undef CHECK_BOXES_MOVED
#undef CHECK_ANY_VISITORS
#undef CHECK_ALL_TOLD
#undef CREATE_VISITOR_STRING

void HandleUseExpiredRepel(void)
{
    VarSet(VAR_REPEL_STEP_COUNT, ItemId_GetHoldEffectParam(VarGet(VAR_LAST_REPEL_USED)));
}

void DetermineCeruleanCaveLayout(void)
{
    u32 trainerId = GetPlayerTrainerId();
    u8 result = trainerId % 3;
    gSpecialVar_Result = result;
}

void CheckTrainerCardStars(void)
{
    u8 stars = 0;

    if(FlagGet(FLAG_SYS_GAME_CLEAR))
    {
        stars++;
    }
    if(HasAllKantoMonsNew())
    {
        stars++;
    }
    if(HasAllMonsNew())
    {
        stars++;
    }
    if((gSaveBlock2Ptr->berryPick.berriesPicked >= 200 && gSaveBlock2Ptr->pokeJump.jumpsInRow >= 200) || gSaveBlock2Ptr->battleTower.bestBattleTowerWinStreak > 49)
    {
        stars++;
    }
    gSpecialVar_Result = stars;
}

#define HAS_TICKETS 16
#define NEEDS_SHOW_EON 17
#define NEEDS_SHOW_AURORA 18
#define NEEDS_SHOW_MYSTIC 19
#define NEEDS_SHOW_OLD_SEA_MAP 20
#define HAS_NO_TICKETS 21

void CheckEventTickets(void)
{
    bool8 haveEonTicket     = CheckBagHasItem(ITEM_EON_TICKET, 1);
    bool8 haveAuroraTicket  = CheckBagHasItem(ITEM_AURORA_TICKET, 1);
    bool8 haveMysticTicket  = CheckBagHasItem(ITEM_MYSTIC_TICKET, 1);
    bool8 haveOldSeaMap     = CheckBagHasItem(ITEM_OLD_SEA_MAP, 1);

    bool8 shownEonTicket    = FlagGet(FLAG_SHOWN_EON_TICKET);
    bool8 shownAuroraTicket = FlagGet(FLAG_SHOWN_AURORA_TICKET);
    bool8 shownMysticTicket = FlagGet(FLAG_SHOWN_MYSTIC_TICKET);
    bool8 shownOldSeaMap    = FlagGet(FLAG_SHOWN_OLD_SEA_MAP);

    u8 multichoiceCase = 0;

    if(gSpecialVar_Result == 0) //checking for showing tickets for the first time
    {
        if(shownEonTicket && shownAuroraTicket && shownMysticTicket && shownOldSeaMap)
        {
            gSpecialVar_Result = HAS_TICKETS;
            return;
        }
        if(haveEonTicket && !shownEonTicket)
        {
            gSpecialVar_Result = NEEDS_SHOW_EON;
            return;
        }
        if(haveAuroraTicket && !shownAuroraTicket)
        {
            gSpecialVar_Result = NEEDS_SHOW_AURORA;
            return;
        }
        if(haveMysticTicket && !shownMysticTicket)
        {
            gSpecialVar_Result = NEEDS_SHOW_MYSTIC;
            return;
        }
        if(haveOldSeaMap && !shownOldSeaMap)
        {
            gSpecialVar_Result = NEEDS_SHOW_OLD_SEA_MAP;
            return;
        }
        if(shownEonTicket || shownAuroraTicket || shownMysticTicket || shownOldSeaMap)
        {
            gSpecialVar_Result = HAS_TICKETS;
            return;
        }
        gSpecialVar_Result = HAS_NO_TICKETS;
        return;
    }
    if(gSpecialVar_Result == 1) //checking which multichoice combo to display
    {
        if(haveEonTicket && shownEonTicket)
        {
            multichoiceCase |= 1 << 3; //setting Eon bit
        }
        if(haveAuroraTicket && shownAuroraTicket)
        {
            multichoiceCase |= 1 << 2; //setting Aurora bit
        }
        if(haveMysticTicket && shownMysticTicket)
        {
            multichoiceCase |= 1 << 1; //setting Mystic bit
        }
        if(haveOldSeaMap && shownOldSeaMap)
        {
            multichoiceCase |= 1 << 0; //setting Old Sea Map bit
        }
        gSpecialVar_Result = multichoiceCase;
        return;
    }
    return;
}

#undef HAS_TICKETS
#undef NEEDS_SHOW_EON
#undef NEEDS_SHOW_AURORA
#undef NEEDS_SHOW_MYSTIC
#undef NEEDS_SHOW_OLD_SEA_MAP
#undef HAS_NO_TICKETS

void RecalculatePartyStats(void)
{
    u32 i;
    for (i = 0; i < gPlayerPartyCount; i++)
    {
        CalculateMonStats(&gPlayerParty[i], FALSE);
    }
}

void ResetTintFilter(void)
{
    u8 val = 0;
    gGlobalFieldTintMode = 0;
    SetInitialPlayerAvatarStateWithDirection(DIR_NORTH);
    StopMapMusic();
    DoMapLoadLoop(&val);
}

void SetLastViewedPokedexEntry(void)
{
    gSaveBlock1Ptr->lastViewedPokedexEntry = GetStarterSpecies();
}

void TurnOffNuzlockeMode(void)
{
    if(gSaveBlock1Ptr->keyFlags.nuzlocke == 1)
    {
        gSaveBlock1Ptr->keyFlags.nuzlocke = 0;
    }
}

void TurnOffNoPMC(void)
{
    if(gSaveBlock1Ptr->keyFlags.noPMC == 1)
    {
        gSaveBlock1Ptr->keyFlags.noPMC = 0;
    }
}

void SetNoPMCTest(void)
{
    u8 noPMC = gSaveBlock1Ptr->keyFlags.noPMC;

    switch(noPMC)
    {
        case 0:
        default:
            gSaveBlock1Ptr->keyFlags.noPMC = 1;
            return;
        case 1:
            gSaveBlock1Ptr->keyFlags.noPMC = 0;
            return;
    }
}

void IsVersionFireRedToVarResult(void)
{
    if(gSaveBlock1Ptr->keyFlags.version == 0)
        gSpecialVar_Result = TRUE;
    else
        gSpecialVar_Result = FALSE;
}

void IsChallengeModeToVarResult(void)
{
    u8 difficulty = gSaveBlock1Ptr->keyFlags.difficulty;
    if(difficulty == DIFFICULTY_CHALLENGE)
    {
        gSpecialVar_Result = TRUE;
    }
    else
    {
        gSpecialVar_Result = FALSE;
    }
}

void FillBagsTest(void)
{
    u32 i;
    //pokeballs
    for(i = 1; i < 13; i++)
    {
        AddBagItem(i, 999);
    }

    //regular items starting with Potion
    for(i = 13; i < 52; i++)
    {
        AddBagItem(i, 999);
    }

    //regular items starting with HP UP, skipping ??????????s
    for(i = 63; i < 87; i++)
    {
        if(i == 72 || i == 82) //skipping random ??????????s
        {
            continue;
        }
        AddBagItem(i, 999);
    }

    //regular items starting with Sun Stone, skipping ??????????s
    for(i = 93; i < 99; i++)
    {
        AddBagItem(i, 999);
    }

    //regular items starting with TinyMushroom, skipping ??????????s
    for(i = 103; i < 112; i++)
    {
        if(i == 105) //skipping random ??????????
        {
            continue;
        }
        AddBagItem(i, 999);
    }

    //regular items starting with Orange Mail
    for(i = 121; i < 133; i++)
    {
        AddBagItem(i, 999);
    }

    //hold items starting with Brightpowder
    for(i = 179; i < 226; i++)
    {
        AddBagItem(i, 999);
    }

    //Contest Scarves (skipping a bunch of ??????????s)
    for(i = 254; i < 259; i++)
    {
        AddBagItem(i, 999);
    }

    //RSE key items that get used in FRLG, starting with Coin Case
    for(i = 260; i < 266; i++)
    {
        AddBagItem(i, 1);
    }

    //FRLG key items starting with Oak's Parcel
    for(i = 349; i < 375; i++)
    {
        AddBagItem(i, 1);
    }

    //berries
    for(i = 133; i < 176; i++)
    {
        AddBagItem(i, 999);
    }

    //TMs and HMs
    for(i = 289; i < 347; i++)
    {
        AddBagItem(i, 999);
    }

    //Old Sea Map
    AddBagItem(376, 1);

    //Link Bracelet
    AddBagItem(112, 1);
}
