/*
    Scene Skipper.

    Skips, with one key, the scenes the game lets the player skip and the
    scenes it does not, and shows an on-screen prompt naming the key while a
    skip is offered.

    Settings are read from scene_skipper.ini in the mod's folder, which the Mod
    Menu Framework's page for the mod (scene_skipper.menupage) writes. A change
    to the file is taken up while the game runs. The log, scene_skipper.log in
    the mod's Logs folder, holds only failures; the previous run's log is moved
    into the Archive folder beside it. What the Scene Skipper is doing can be
    asked through its export SceneSkipperDescribe, which the Dev Toolkit's
    [tasks] watchSceneSkipper logs.

    The addresses below are those of tlou-i.exe 1.1.5.0. On any other build
    the Scene Skipper does nothing and says so in its log. A function that
    carries a signature is found by it.

    Everything happens in the game's script update (ScriptManager::
    ListenerUpdate), on the game's own thread, except the registration of the
    prompt's widget files, which happens while the game loads its widget files
    a few seconds after it starts.

    The skip
    --------
    While a skip is offered, the skip key makes it the way the pause menu's
    Skip Cinematic makes one once Yes is chosen: the event cinematic-skip is
    broadcast while a cinematic plays, carrying where the skip goes, and three
    ticks later GameTaskSubnode::SkipTo plays the target from its checkpoint.
    A skip is offered:

      - while a cinematic plays that the scene table names: to the table's
        target;
      - while any other cinematic plays that the game offers Skip Cinematic
        for: to the game's own target, where the pause menu would go;
      - while no cinematic gives one, during a scene the table names that is
        played in-game (the phone, the study, the car ride): to the table's
        target.

    The key during the first cinematic goes past the phone after it, to where
    Sarah stands up and the player first has control.

    The key does nothing while an in-game menu (the pause menu and those it
    opens) is up, as the game itself asks each frame, so a key held to bind it
    in the menu makes no skip.

    The speedrun timer
    ------------------
    The game's speedrun timer counts the scenes played in-game (the phone,
    the study, the car ride) but not cinematics. In speedrun mode a skip of
    such a scene adds to the timer the time it left out, so a run reads the
    same with skips as without.

    The pause menu
    --------------
    During the scenes the table names where no cinematic plays (the phone, the
    study and the car ride), the pause menu offers its own Skip Cinematic row,
    as it does during a cinematic, and Yes in its box makes the skip the key
    makes. The row and the box's title then read the prompt's text for the
    scene in capitals (SKIP SCENE, SKIP CAR RIDE), and the box asks the
    table's question for the scene, or the game's own with the scene named
    where it names a cinematic. During a cinematic the row, its words and its
    skip are the game's own.

    During the daughter's death a press moves the death's cinematic on to the
    game's title card (frame 3399) instead of leaving it, ending the audio it
    was playing first; from the card on the scene counts as the titles, so the
    prompt reads Skip Titles and the key goes past the titles to the 20 years
    later cinematic.

    The prompt
    ----------
    The prompt is a widget of the Scene Skipper's own: one widget file for each
    size from 50% to 200% in steps of 10, written here as JSON in the engine's
    widget document schema and put in the game's widget file table while the
    game loads its widget files. It holds the skip key's icon and the scene's
    text in the look of the game's own button prompts, sits in a corner of the
    screen, and is kept in the title-cards category, the one the game still
    draws while a cinematic or a movie hides the rest of the interface. It
    plays no sound. When a skip comes to be offered it fades in to its opacity,
    stays its duration and fades out; the key still skips after it has gone.
*/

#include <windows.h>
#include <intrin.h>
#include <cstdarg>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#undef near
#undef far

#define TLOU_PLUGIN
#include "tlou_plugin_sdk.h"

#ifdef _MSC_VER
#define GUARD_TRY    __try
#define GUARD_EXCEPT __except (EXCEPTION_EXECUTE_HANDLER)
#else
#define GUARD_TRY    if (1)
#define GUARD_EXCEPT else
#endif

static const char* const modBaseName = "scene_skipper";

/* ---------------------------------------------------------------- the log */

static char global_logPath[MAX_PATH];
static char global_iniPath[MAX_PATH];

static void writeLog(const char* fmt, ...) {
    if (!global_logPath[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, global_logPath, "a") != 0 || !f) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u  ",
            now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* Formats into out. Answers 0 when the result does not fit in room, and out is
   then empty. */
static int formatInto(char* out, size_t room, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int written = _vsnprintf_s(out, room, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (written >= 0) return 1;
    out[0] = 0;
    return 0;
}

/* The log of the previous run, moved into the Archive folder beside it and
   named for the time it was last written. */
static void archiveLog() {
    WIN32_FILE_ATTRIBUTE_DATA about;
    if (!global_logPath[0]
        || !GetFileAttributesExA(global_logPath, GetFileExInfoStandard, &about))
        return;
    if (!about.nFileSizeLow && !about.nFileSizeHigh) return;

    SYSTEMTIME written, when;
    FileTimeToSystemTime(&about.ftLastWriteTime, &written);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &written, &when)) {
        FILETIME local;
        FileTimeToLocalFileTime(&about.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &when);
    }

    char logs[MAX_PATH], folder[MAX_PATH], kept[MAX_PATH];
    formatInto(logs, sizeof(logs), "%s", global_logPath);
    char* slash = strrchr(logs, '\\');
    if (!slash) return;
    *slash = 0;
    if (!formatInto(folder, sizeof(folder), "%s\\Archive", logs)) return;
    CreateDirectoryA(folder, nullptr);
    if (!formatInto(kept, sizeof(kept), "%s\\%s_%04u%02u%02u_%02u%02u%02u.log",
                    folder, modBaseName, when.wYear, when.wMonth, when.wDay,
                    when.wHour, when.wMinute, when.wSecond))
        return;
    MoveFileA(global_logPath, kept);
}

/* ------------------------------------------------------- reading the game */

#define SMALLEST_POINTER 0x10000ull
#define LARGEST_POINTER  0x00007FFFFFFFFFFFull   /* the top of the user half */

static int readQword(uint64_t address, uint64_t* out) {
    if (address < SMALLEST_POINTER || address > LARGEST_POINTER) return 0;
    int ok = 0;
    GUARD_TRY { *out = *(const uint64_t*)address; ok = 1; }
    GUARD_EXCEPT { ok = 0; }
    return ok;
}

static int readDword(uint64_t address, unsigned int* out) {
    if (address < SMALLEST_POINTER || address > LARGEST_POINTER) return 0;
    int ok = 0;
    GUARD_TRY { *out = *(const unsigned int*)address; ok = 1; }
    GUARD_EXCEPT { ok = 0; }
    return ok;
}

static int readByte(uint64_t address, unsigned char* out) {
    if (address < SMALLEST_POINTER || address > LARGEST_POINTER) return 0;
    int ok = 0;
    GUARD_TRY { *out = *(const unsigned char*)address; ok = 1; }
    GUARD_EXCEPT { ok = 0; }
    return ok;
}

static int readFloat(uint64_t address, float* out) {
    unsigned int bits = 0;
    if (!readDword(address, &bits)) return 0;
    memcpy(out, &bits, sizeof(*out));
    return 1;
}

static int writeDword(uint64_t address, unsigned int value) {
    if (address < SMALLEST_POINTER || address > LARGEST_POINTER) return 0;
    int ok = 0;
    GUARD_TRY { *(unsigned int*)address = value; ok = 1; }
    GUARD_EXCEPT { ok = 0; }
    return ok;
}

static int writeByte(uint64_t address, unsigned char value) {
    if (address < SMALLEST_POINTER || address > LARGEST_POINTER) return 0;
    int ok = 0;
    GUARD_TRY { *(unsigned char*)address = value; ok = 1; }
    GUARD_EXCEPT { ok = 0; }
    return ok;
}

static int writeQword(uint64_t address, uint64_t value) {
    if (address < SMALLEST_POINTER || address > LARGEST_POINTER) return 0;
    int ok = 0;
    GUARD_TRY { *(uint64_t*)address = value; ok = 1; }
    GUARD_EXCEPT { ok = 0; }
    return ok;
}

static uint64_t global_moduleBase = 0;
static uint64_t global_moduleSize = 0;
static uint64_t global_textBase = 0;
static uint64_t global_textSize = 0;

static int inCode(uint64_t address) {
    return address >= global_textBase
        && address < global_textBase + global_textSize;
}

static int inModule(uint64_t address) {
    return address >= global_moduleBase
        && address < global_moduleBase + global_moduleSize;
}

/* FNV-1a 64. Every id in the engine is this hash of a name. */
static uint64_t idOfName(const char* text) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (const unsigned char* at = (const unsigned char*)text; *at; ++at) {
        hash ^= (uint64_t)*at;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

/* ------------------------------------------------- the game's own parts */

/*
    tlou-i.exe 1.1.5.0, by its time stamp. Every address below is that build's.
*/
#define GAME_BUILD_STAMP           0x685BEBE1u

#define TASK_MANAGER_RVA           0x047291D0ull   /* the task manager */
#define CURRENT_TASK_NODE_RVA      0x005EBC10ull   /* (manager) -> the current task's node */
#define CURRENT_GRAPH_RVA          0x005EBE70ull   /* (manager) -> the running task graph */
#define FIND_NODE_RVA              0x006ADB70ull   /* (graph, name, 1 by name / 0 by full name) */
#define CINEMATIC_MANAGER_RVA      0x068FA658ull   /* -> the cinematic manager */
#define SKIP_TARGET_RVA            0x0237DB60ull   /* (manager, &skip to, &checkpoint) -> node */
#define SKIP_OFFERED_RVA           0x0237E93Cull   /* (manager): the game offers Skip Cinematic */
#define BROADCAST_EVENT_RVA        0x023A9BACull   /* (script manager, event, four arguments) */
#define STATE_SCRIPT_MANAGER_RVA   0x067E3280ull
#define KEYBOARD_STATE_RVA         0x07FE7E00ull   /* the frame's copy of GetKeyboardState */
#define HUD_HANDLE_RVA             0x06889AA0ull   /* the interface root's handle */
#define MENU_ROOT_HANDLE_RVA       0x067A6C10ull   /* the menu root's handle (023712A4) */
#define AS_MENU_ROOT_RVA           0x01C8A3B8ull   /* (object) -> it as Menu2::RootGame, or 0 */
#define MENU_ROOT_IN_MENU_SLOT     0x470           /* (root, 0): an in-game menu is up */

/* A task node. */
#define NODE_NAME            0x08
#define NODE_FULL_NAME       0x28

/* A handle is a heap record and an id; the record holds the object. */
#define HANDLE_RECORD        0x00
#define HANDLE_ID            0x08
#define RECORD_OBJECT        0x08
#define PROCESS_ID           0x98

/* The cinematic manager, and a cinematic process. */
#define MANAGER_CINEMATIC    0xA8
#define CINEMATIC_NAME       0x1E0
#define CINEMATIC_TASK       0x1E8

/* An argument of an event: a value and its type. */
#define BOXED_BOOL           1
#define BOXED_FLOAT          6
#define BOXED_NAME           7
#define BOXED_STRING         0x13

struct BoxedValue {
    uint64_t value;
    uint32_t unused;
    uint32_t type;
};

typedef uint64_t (*CurrentTaskFn)(uint64_t);
typedef uint64_t (*CurrentGraphFn)(uint64_t);
typedef uint64_t (*FindNodeFn)(uint64_t, uint64_t, int);
typedef unsigned char (*SkipToFn)(uint64_t, uint64_t);
typedef uint64_t (*SkipTargetFn)(uint64_t, uint64_t*, uint64_t*);
typedef unsigned char (*SkipOfferedFn)(uint64_t);
typedef void (*BroadcastFn)(uint64_t, uint64_t, BoxedValue*, BoxedValue*,
                            BoxedValue*, BoxedValue*);
typedef uint64_t (*AsMenuRootFn)(uint64_t);
typedef unsigned char (*InMenuFn)(uint64_t, int);

static const char* const skipToSignature =
    "bool __cdecl GameTaskSubnode::SkipTo(class StringId64) const";
static const char* const listenerUpdateSignature =
    "void __cdecl ScriptManager::ListenerUpdate(void)";
static const char* const parseDataSignature =
    "class jdom::Static::ValuePtr __cdecl jdom::ParseData(const char *,unsigned __int64,class HeapAllocator *)";
static const char* const loadAnimationsSignature =
    "void __cdecl Gui2::WidgetFile::LoadAnimations(class jdom::Static::ValuePtr)";

static int global_gameKnown = 0;   /* 1 on the build the addresses are for */

/* The process a handle names, or 0 when it names nothing now. */
static uint64_t objectOfHandle(uint64_t handle) {
    uint64_t record = 0, object = 0;
    unsigned id = 0, idNow = 0;
    if (!readQword(handle + HANDLE_RECORD, &record) || !record) return 0;
    if (!readDword(handle + HANDLE_ID, &id) || !id) return 0;
    if (!readQword(record + RECORD_OBJECT, &object) || !object) return 0;
    if (!readDword(object + PROCESS_ID, &idNow) || idNow != id) return 0;
    return object;
}

/*
    The function a signature names a part of, at its entry: the exception
    directory's chain back to the function's first part.
*/
static uint64_t entryOfFunction(uint64_t address) {
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION entry =
        RtlLookupFunctionEntry((DWORD64)address, &imageBase, nullptr);
    for (int depth = 0; entry && depth < 16; ++depth) {
        const DWORD unwind = entry->UnwindData;
        if (unwind & 1u) {
            entry = (PRUNTIME_FUNCTION)(imageBase + (unwind & ~1u));
            continue;
        }
        const uint8_t* info = (const uint8_t*)(imageBase + unwind);
        const uint8_t flags = (uint8_t)(info[0] >> 3);
        if (!(flags & UNW_FLAG_CHAININFO)) break;
        const unsigned codes = info[2];
        entry = (PRUNTIME_FUNCTION)(info + 4 + ((codes + 1u) & ~1u) * 2u);
    }
    return entry ? (uint64_t)(imageBase + entry->BeginAddress) : 0;
}

/* --------------------------------------------------------- the settings */

#define PROMPT_SIZE_LEAST        50      /* skip-prompt-size, a percentage, */
#define PROMPT_SIZE_MOST         200     /* in steps of PROMPT_SIZE_STEP: */
#define PROMPT_SIZE_STEP         10      /* one widget file each */
#define PROMPT_SIZES             (((PROMPT_SIZE_MOST - PROMPT_SIZE_LEAST) / PROMPT_SIZE_STEP) + 1)
#define PROMPT_LONGEST_SECONDS   30      /* skip-prompt-duration 1 to 30: held that long */
#define PROMPT_PERSISTENT        31      /* above 30: held while the skip is offered */

/*
    enabled     enable-scene-skipper: 0 and the Scene Skipper does nothing
    key         skip-button, a Windows virtual-key code written in hexadecimal
                (F9 is 0x78); 0 for none
    showPrompt  show-skip-prompt: 0 hides the prompt, the key still skips
    corner      skip-prompt-corner: 1 top left, 2 top right, 3 bottom left,
                4 bottom right
    size        skip-prompt-size, 50 to 200 in steps of 10
    opacity     skip-prompt-opacity, 10 to 100
    seconds     skip-prompt-duration: 0 never shown, 1 to 30 held that long,
                31 (Persistent) held while the skip is offered
*/
struct Settings {
    int enabled;
    int key;
    int showPrompt;
    int corner;
    int size;
    int opacity;
    int seconds;
};

static Settings global_set = { 1, 0x78, 1, 3, 100, 60, 10 };

static int clampInt(int value, int least, int most) {
    return value < least ? least : value > most ? most : value;
}

static void readSettings(Settings* s) {
    s->enabled = GetPrivateProfileIntA("settings", "enable-scene-skipper", 1,
                                       global_iniPath) != 0;
    char text[32];
    GetPrivateProfileStringA("settings", "skip-button", "0x78", text,
                             (DWORD)sizeof(text), global_iniPath);
    char* end = nullptr;
    const unsigned long key = strtoul(text, &end, 16);
    s->key = (end && end != text && key > 0 && key < 0xFF) ? (int)key : 0;
    if (!s->key)
        writeLog("SETTINGS skip-button \"%s\" is not a key's virtual-key code "
                 "in hexadecimal, so the skip key is off", text);
    s->showPrompt = GetPrivateProfileIntA("settings", "show-skip-prompt", 1,
                                          global_iniPath) != 0;
    s->corner = clampInt(GetPrivateProfileIntA("settings", "skip-prompt-corner",
                                               3, global_iniPath), 1, 4);
    int size = GetPrivateProfileIntA("settings", "skip-prompt-size", 100,
                                     global_iniPath);
    size = (size + PROMPT_SIZE_STEP / 2) / PROMPT_SIZE_STEP * PROMPT_SIZE_STEP;
    s->size = clampInt(size, PROMPT_SIZE_LEAST, PROMPT_SIZE_MOST);
    s->opacity = clampInt(GetPrivateProfileIntA("settings",
                                                "skip-prompt-opacity", 60,
                                                global_iniPath), 10, 100);
    s->seconds = clampInt(GetPrivateProfileIntA("settings",
                                                "skip-prompt-duration", 10,
                                                global_iniPath),
                          0, PROMPT_PERSISTENT);
}

/* The key makes skips. */
static int keyOn(void) {
    return global_set.enabled && global_set.key;
}

/* The prompt is shown while a skip is offered. */
static int promptOn(void) {
    return keyOn() && global_set.showPrompt && global_set.seconds > 0;
}

/*
    The settings are read again only once the ini's last-write time has stayed
    put across a further look, so a file the menu is still writing is not read
    half written.
*/
#define SETTINGS_LOOK_EVERY_MS 250u

static FILETIME global_iniReadAt;      /* the write time the settings came from */
static FILETIME global_iniSettlingAt;  /* a newer write time, not yet settled */
static int      global_iniSettling = 0;
static DWORD    global_lastLookAt = 0;

static int iniWrittenAt(FILETIME* out) {
    WIN32_FILE_ATTRIBUTE_DATA about;
    if (!GetFileAttributesExA(global_iniPath, GetFileExInfoStandard, &about))
        return 0;
    *out = about.ftLastWriteTime;
    return 1;
}

static int sameTime(const FILETIME* a, const FILETIME* b) {
    return a->dwLowDateTime  == b->dwLowDateTime
        && a->dwHighDateTime == b->dwHighDateTime;
}

/* 1 on the look that settles a new write, with the new settings in *out. */
static int settingsChanged(Settings* out) {
    const DWORD now = GetTickCount();
    if (now - global_lastLookAt < SETTINGS_LOOK_EVERY_MS) return 0;
    global_lastLookAt = now;

    FILETIME written;
    if (!iniWrittenAt(&written)) return 0;
    if (sameTime(&written, &global_iniReadAt)) {
        global_iniSettling = 0;
        return 0;
    }
    if (!global_iniSettling || !sameTime(&written, &global_iniSettlingAt)) {
        global_iniSettlingAt = written;
        global_iniSettling = 1;
        return 0;
    }
    global_iniReadAt = written;
    global_iniSettling = 0;
    readSettings(out);
    return 1;
}

/* ------------------------------------------------------- the scene table */

/*
    The scenes the Scene Skipper adds a skip to, by the task's own name (the
    titles are hom-credits, not home-hom-credits), with the task a skip made
    during each goes to and the prompt's text for it. A scene not here reads
    Skip Cinematic while a cinematic plays, else Skip Scene. Where the pause
    menu offers the Scene Skipper's skip (a scene here with no cinematic
    playing), question is what its confirm box asks; null asks the game's own
    question with the prompt's noun in place of cinematic (Skip this scene?).

    The first cinematic and the phone: the game's own Skip Cinematic of the
    first cinematic (hom-intro) goes to the phone (hom-house-daughter-room-
    phone), an in-game scene the player cannot skip or play, in which Sarah
    answers the phone and gets out of bed; the player first has control when
    she stands up, which is where hom-house-daughter-room begins (the phone's
    script, ss-hom-house-daughter-room, completes the phone's task as the
    scene's camera hands back and then waits for the player in the room). The
    key skips the cinematic, or the phone, to there; the pause menu's skip of
    the cinematic stays the game's own.

    The study: Sarah's search of the study, which the player cannot leave, is
    skipped together with the escape cinematic after it, to the car ride. A
    skip landing between them lost the escape's first voices, and the escape's
    own restart goes back to Sarah entering the study anyway.

    The car ride, in the segments the game plays it as: the game never offers
    Skip Cinematic during it. The skip goes to the crash's rescue.

    The death, the titles and the 20 years later cinematic: the game's own Skip
    Cinematic goes from any of them straight past 20 years later; these let
    each be skipped on its own.

    inGame is 1 for a scene played in-game (the phone, the study, the car
    ride), offered whenever its task is the current one; a cinematic's scene
    (0) is offered only while its cinematic plays, so the moments its task is
    current without it (just before it starts, just after the game's own skip
    ends it) offer nothing.

    seconds is the speedrun timer's time (it stops during cinematics and while
    the screen is fully faded) from the scene's start to the target's start
    in a playthrough with no skips, which a skip adds back (the speedrun
    timer, below). Test run 16 (2026-10-02): the phone 30.975 s, the study
    30.598 s, the car ride 173.351 s from its start. A start part way through
    the ride takes its segment's remainder, from the segments' times of
    2026-10-01 (34.297, 53.601, 76.938 and 117.779 s into the ride). The first
    cinematic's is the phone's, which its skip passes over; the timer does not
    count the cinematic itself.
*/
struct SceneSkip {
    const char* scene;
    const char* target;
    const char* label;     /* null for the default text */
    const char* question;  /* the pause menu's confirm box; null for the game's, reworded */
    int         inGame;    /* 1 played in-game; 0 a cinematic's, offered only while it plays */
    double      seconds;   /* the speedrun timer's time from the scene's start to the target's */
    uint64_t    sceneId;
    uint64_t    targetId;
};

#define CAR_RIDE_QUESTION "Skip the car ride?"

static SceneSkip global_sceneTable[] = {
    { "hom-intro",                     "hom-house-daughter-room",     nullptr,         nullptr,           0,  30.975, 0, 0 },
    { "hom-house-daughter-room-phone", "hom-house-daughter-room",     nullptr,         nullptr,           1,  30.975, 0, 0 },
    { "hom-house-study",               "hom-road-suburbs-start",      nullptr,         nullptr,           1,  30.598, 0, 0 },
    { "hom-road-suburbs-start",        "hom-town-crash-rescue-intro", "Skip Car Ride", CAR_RIDE_QUESTION, 1, 173.351, 0, 0 },
    { "hom-road-suburbs-intersection", "hom-town-crash-rescue-intro", "Skip Car Ride", CAR_RIDE_QUESTION, 1, 139.054, 0, 0 },
    { "hom-road-woods-start",          "hom-town-crash-rescue-intro", "Skip Car Ride", CAR_RIDE_QUESTION, 1, 119.750, 0, 0 },
    { "hom-road-highway",              "hom-town-crash-rescue-intro", "Skip Car Ride", CAR_RIDE_QUESTION, 1,  96.413, 0, 0 },
    { "hom-road-freeway-traffic",      "hom-town-crash-rescue-intro", "Skip Car Ride", CAR_RIDE_QUESTION, 1,  55.572, 0, 0 },
    { "hom-losing-daughter",           "hom-credits",                 nullptr,         nullptr,           0,   0.0,   0, 0 },
    { "hom-credits",                   "mil-20years-later",           "Skip Titles",   nullptr,           0,   0.0,   0, 0 },
    { "mil-20years-later",             "mil-street-start",            nullptr,         nullptr,           0,   0.0,   0, 0 },
};

#define SCENE_COUNT ((int)(sizeof(global_sceneTable) / sizeof(global_sceneTable[0])))

/*
    Skips that move the cinematic playing on to a frame of it instead of
    leaving it. During the scene, while the cinematic plays and is more than
    FRAME_SKIP_MARGIN frames short of the frame, the key moves it there; from
    the frame on (less the margin), whether a move took it there or it played
    there, the scene counts as the one named after: its text and its target.

    The death lands at frame 3399, where its script, ss-movie-hom-mil-
    transition, shows the game's title card for the death's last 4.7 s before
    the titles; the card then counts as the titles.
*/
struct FrameSkip {
    const char* scene;
    const char* cinematic;
    float       frame;
    const char* after;
    uint64_t    sceneId;
    uint64_t    cinematicId;
    uint64_t    afterId;
};

static FrameSkip global_frameTable[] = {
    { "hom-losing-daughter", "cin-hom-losing-daughter", 3399.0f, "hom-credits", 0, 0, 0 },
};

#define FRAME_SKIP_COUNT ((int)(sizeof(global_frameTable) / sizeof(global_frameTable[0])))

static void nameTheTables(void) {
    for (int i = 0; i < SCENE_COUNT; ++i) {
        global_sceneTable[i].sceneId = idOfName(global_sceneTable[i].scene);
        global_sceneTable[i].targetId = idOfName(global_sceneTable[i].target);
    }
    for (int i = 0; i < FRAME_SKIP_COUNT; ++i) {
        global_frameTable[i].sceneId = idOfName(global_frameTable[i].scene);
        global_frameTable[i].cinematicId = idOfName(global_frameTable[i].cinematic);
        global_frameTable[i].afterId = idOfName(global_frameTable[i].after);
    }
}

static const SceneSkip* sceneSkipFor(uint64_t scene) {
    for (int i = 0; i < SCENE_COUNT; ++i)
        if (global_sceneTable[i].sceneId == scene) return &global_sceneTable[i];
    return nullptr;
}

static const FrameSkip* frameSkipFor(uint64_t scene) {
    for (int i = 0; i < FRAME_SKIP_COUNT; ++i)
        if (global_frameTable[i].sceneId == scene) return &global_frameTable[i];
    return nullptr;
}

/* ------------------------------------------------------------ the scene */

static uint64_t global_currentTask = 0;   /* this tick's current task node */

static uint64_t currentTaskNode(void) {
    uint64_t node = 0;
    GUARD_TRY {
        node = ((CurrentTaskFn)(global_moduleBase + CURRENT_TASK_NODE_RVA))(
            global_moduleBase + TASK_MANAGER_RVA);
    } GUARD_EXCEPT { node = 0; }
    return node;
}

/* The running task graph's node with this name (byName 1) or full name
   (byName 0), or 0. */
static uint64_t nodeNamed(uint64_t name, int byName) {
    uint64_t node = 0;
    GUARD_TRY {
        const uint64_t graph = ((CurrentGraphFn)(global_moduleBase
                                                 + CURRENT_GRAPH_RVA))(
            global_moduleBase + TASK_MANAGER_RVA);
        if (graph)
            node = ((FindNodeFn)(global_moduleBase + FIND_NODE_RVA))(graph,
                                                                    name,
                                                                    byName);
    } GUARD_EXCEPT { node = 0; }
    return node;
}

/* The cinematic the cinematic manager plays now, or 0. */
static uint64_t currentCinematicProcess(void) {
    uint64_t manager = 0;
    if (!readQword(global_moduleBase + CINEMATIC_MANAGER_RVA, &manager)
        || !manager)
        return 0;
    return objectOfHandle(manager + MANAGER_CINEMATIC);
}

/* The task node of the cinematic playing now, or 0. A cinematic holds its
   task by full name; the titles play under hom-credits while the task
   manager still shows the death, so this is the surer name for the scene. */
static uint64_t currentCinematicTaskNode(void) {
    const uint64_t process = currentCinematicProcess();
    if (!process) return 0;
    uint64_t taskFullName = 0;
    if (!readQword(process + CINEMATIC_TASK, &taskFullName) || !taskFullName)
        return 0;
    return nodeNamed(taskFullName, 0);
}

/*
    Where the pause menu's Skip Cinematic would go now, as the game decides
    it: the cinematic manager's SkipTarget, when SkipOffered answers true. The
    target task's node, with its checkpoint in *checkpoint; 0 when the game
    offers no skip.
*/
static uint64_t skipTheGameOffers(uint64_t* checkpoint) {
    uint64_t manager = 0, skipTo = 0, answer = 0, node = 0;
    unsigned char offered = 0;
    *checkpoint = 0;
    if (!global_currentTask) return 0;
    if (!readQword(global_moduleBase + CINEMATIC_MANAGER_RVA, &manager)
        || !manager)
        return 0;
    GUARD_TRY {
        offered = ((SkipOfferedFn)(global_moduleBase + SKIP_OFFERED_RVA))(
                      manager) & 1;
        if (offered)
            node = ((SkipTargetFn)(global_moduleBase + SKIP_TARGET_RVA))(
                manager, &skipTo, &answer);
    } GUARD_EXCEPT { offered = 0; node = 0; }
    if (!offered || !node) return 0;
    *checkpoint = answer;
    return node;
}

/* 1 while an in-game menu is up (the pause menu and the pages it opens, not
   photo mode), as the game asks each frame before it hides the interface for
   a cinematic: the menu root's slot 142. */
static int inGameMenuUp(void) {
    const uint64_t object = objectOfHandle(global_moduleBase
                                           + MENU_ROOT_HANDLE_RVA);
    if (!object) return 0;
    unsigned char up = 0;
    GUARD_TRY {
        const uint64_t root = ((AsMenuRootFn)(global_moduleBase
                                              + AS_MENU_ROOT_RVA))(object);
        uint64_t table = 0, method = 0;
        if (root && readQword(root, &table)
            && readQword(table + MENU_ROOT_IN_MENU_SLOT, &method)
            && inCode(method))
            up = ((InMenuFn)method)(root, 0);
    } GUARD_EXCEPT { up = 0; }
    return up != 0;
}

/* ------------------------------------------------- the move to a frame */

/*
    The frames are the scripts': wait-until-cin-frame waits until
    cinematic-get-current-frame reaches its frame, and that is the time in
    seconds of the cinematic's master animation (+0x450) times 30, the master
    animation being 024E6600's answer for the handle at the cinematic's
    +0x298. 024EBECC(cinematic, seconds) marks the cinematic with the time to
    move to (+0x500, -FLT_MAX when none is pending), and at its next update the
    cinematic sends each of its actors the time and starts its audio tracks
    again from it.

    The audio the cinematic was playing is ended first, as the engine ends it
    when it kills a cinematic: 024E5A80 with force. The update's own stop does
    nothing while an actor's animation has ended (+0x52A) and the cinematic's
    audio may play on past its end (+0x529, 1 by default), and the old sounds
    then play on beside the new; so +0x52A is cleared for the call and put
    back after.
*/
#define CINEMATIC_STOP_AUDIO_RVA 0x024E5A80ull  /* (cinematic, force) */
#define CINEMATIC_ANIM_ENDED     0x52A          /* byte: an actor's animation has ended */
#define CINEMATIC_SEEK_RVA       0x024EBECCull  /* (cinematic, seconds) */
#define CINEMATIC_MASTER_RVA     0x024E6600ull  /* (cinematic, &its +0x298) -> its master animation */
#define CINEMATIC_MASTER_HANDLE  0x298
#define MASTER_TIME              0x450          /* float, seconds */
#define CINEMATIC_SEEK_PENDING   0x500          /* float seconds; -FLT_MAX when none */
#define FRAME_SKIP_MARGIN        15             /* frames short of the landing still moved */

typedef void (*CinematicSeekFn)(uint64_t, float);
typedef uint64_t (*CinematicMasterFn)(uint64_t, const void*);
typedef void (*CinematicStopAudioFn)(uint64_t, char);

/* A cinematic's time in seconds, from its master animation; 0 when it has
   none. */
static int cinematicTime(uint64_t cinematic, float* time) {
    uint64_t master = 0;
    GUARD_TRY {
        master = ((CinematicMasterFn)(global_moduleBase + CINEMATIC_MASTER_RVA))(
            cinematic, (const void*)(uintptr_t)(cinematic
                                                + CINEMATIC_MASTER_HANDLE));
    } GUARD_EXCEPT { master = 0; }
    return master && readFloat(master + MASTER_TIME, time);
}

/*
    The frame skip's cinematic, when it is the one playing: its process and
    its frame, a move still pending counting as made. 0 when it is not playing
    or its frame cannot be read.
*/
static uint64_t frameSkipCinematic(const FrameSkip* landing, float* frame) {
    *frame = 0.0f;
    const uint64_t cinematic = currentCinematicProcess();
    uint64_t playing = 0;
    float time = 0.0f, pending = 0.0f;
    if (!cinematic || !readQword(cinematic + CINEMATIC_NAME, &playing)
        || playing != landing->cinematicId || !cinematicTime(cinematic, &time))
        return 0;
    if (readFloat(cinematic + CINEMATIC_SEEK_PENDING, &pending)
        && pending > time && pending < 1.0e6f)
        time = pending;
    *frame = time * 30.0f;
    return cinematic;
}

static int frameSkipLanded(const FrameSkip* landing) {
    float frame = 0.0f;
    return frameSkipCinematic(landing, &frame)
        && frame >= landing->frame - (float)FRAME_SKIP_MARGIN;
}

/* Ends the audio a cinematic is playing, whatever its flags. */
static int stopCinematicAudio(uint64_t cinematic) {
    unsigned char ended = 0;
    readByte(cinematic + CINEMATIC_ANIM_ENDED, &ended);
    if (ended) writeByte(cinematic + CINEMATIC_ANIM_ENDED, 0);
    int ok = 1;
    GUARD_TRY {
        ((CinematicStopAudioFn)(global_moduleBase + CINEMATIC_STOP_AUDIO_RVA))(
            cinematic, 1);
    } GUARD_EXCEPT { ok = 0; }
    if (ended) writeByte(cinematic + CINEMATIC_ANIM_ENDED, ended);
    return ok;
}

/* ------------------------------------------------------- what is offered */

#define SKIP_BY_TABLE 1   /* the scene table names the scene */
#define SKIP_BY_GAME  2   /* the game offers Skip Cinematic for the cinematic */
#define SKIP_BY_MOVE  3   /* the cinematic is moved on to a frame of it */

/*
    What a skip asked now would do: 1 with the scene it is made during, the
    task it goes to, its checkpoint, whether a cinematic plays and how the
    target was found; 0 when none would be made, with the scene still given.

    A playing cinematic names its scene surely, so it comes first: the table's
    entry for it, else the target the game's own Skip Cinematic offers. Only
    when the cinematic gives neither is the current task asked. A cinematic at
    or past a frame skip's landing counts as the scene the frame skip names
    after it, and *countedFrom is then the scene the cinematic itself names.
*/
static int resolveSkip(uint64_t* scene, uint64_t* target, uint64_t* checkpoint,
                       int* cinematic, int* how, uint64_t* countedFrom) {
    uint64_t name = 0;
    *scene = *target = *checkpoint = *countedFrom = 0;
    *cinematic = 0;
    *how = 0;
    const uint64_t cineNode = currentCinematicTaskNode();
    if (cineNode && readQword(cineNode + NODE_NAME, &name) && name) {
        *cinematic = 1;
        const FrameSkip* landing = frameSkipFor(name);
        if (landing && landing->afterId && frameSkipLanded(landing)) {
            *countedFrom = name;
            name = landing->afterId;
        }
        *scene = name;
        const SceneSkip* listed = sceneSkipFor(name);
        if (listed) {
            *target = listed->targetId;
            *checkpoint = idOfName("default");
            *how = SKIP_BY_TABLE;
            return 1;
        }
        uint64_t answer = 0, offered = 0;
        const uint64_t node = skipTheGameOffers(&answer);
        if (node && readQword(node + NODE_NAME, &offered) && offered) {
            *target = offered;
            *checkpoint = answer ? answer : idOfName("default");
            *how = SKIP_BY_GAME;
            return 1;
        }
    }
    uint64_t task = 0;
    if (global_currentTask && readQword(global_currentTask + NODE_NAME, &task)
        && task) {
        if (!*scene) *scene = task;
        const SceneSkip* listed = sceneSkipFor(task);
        if (listed && listed->inGame) {
            *scene = task;
            *countedFrom = 0;
            *target = listed->targetId;
            *checkpoint = idOfName("default");
            *how = SKIP_BY_TABLE;
            return 1;
        }
    }
    return 0;
}

/* The frame a press would move the playing cinematic to during scene, or 0
   when a press would not move it. */
static float moveFrameFor(uint64_t scene, int cinematic) {
    if (!cinematic) return 0.0f;
    const FrameSkip* landing = frameSkipFor(scene);
    if (!landing) return 0.0f;
    float frame = 0.0f;
    if (!frameSkipCinematic(landing, &frame)
        || frame >= landing->frame - (float)FRAME_SKIP_MARGIN)
        return 0.0f;
    return landing->frame;
}

/* The prompt's text for a scene. */
static const char* labelFor(uint64_t scene, int cinematic) {
    const SceneSkip* listed = sceneSkipFor(scene);
    if (listed && listed->label) return listed->label;
    return cinematic ? "Skip Cinematic" : "Skip Scene";
}

/* ------------------------------------------------------ the speedrun timer */

/*
    The game's speedrun timer stops while a cinematic plays and while the
    screen is fully faded, and runs through the scenes played in-game: the
    phone, the study and the car ride. A skip of one leaves its time out, so
    a run with skips would read faster than one without. In speedrun mode a
    skip adds that time back: the table's seconds for the scene (the timer's
    time from its start to the target's in a playthrough with no skips) less
    the time the timer has run since the scene was first offered, added to the
    chapter's speedrun time as the target is played, before the game's save
    there, so the save and the speedrun HUD both carry it.

    GameInfo, the pointer at GAME_INFO_RVA: +0x477C speedrun mode, +0x4960 the
    chapter's speedrun time in seconds (a double), which the game adds each
    frame's time to (01CC7C34) and the HUD's total ends with.

    The mark: the chapter's time when a table skip to a target was first
    offered, and that scene's seconds. It is taken again when a skip to
    another target is offered or the chapter's time goes back (a new game, or
    a checkpoint putting back a saved time), and dropped when a skip is made.
    A moment with no skip offered keeps it, so the phone after the first
    cinematic keeps the cinematic's mark: the timer stood still through the
    cinematic, and both name the phone's time.
*/
#define GAME_INFO_RVA            0x06433100ull
#define GAME_INFO_SPEEDRUN_ON    0x477C
#define GAME_INFO_SPEEDRUN_TIME  0x4960

static uint64_t global_markTarget = 0;
static double   global_markChapter = 0.0;
static double   global_markSeconds = 0.0;

static uint64_t gameInfo(void) {
    uint64_t info = 0;
    if (!readQword(global_moduleBase + GAME_INFO_RVA, &info)
        || info < SMALLEST_POINTER || info > LARGEST_POINTER)
        return 0;
    return info;
}

/* The chapter's speedrun time, seconds; 0 when it cannot be read. */
static int speedrunChapter(double* seconds) {
    const uint64_t info = gameInfo();
    uint64_t bits = 0;
    if (!info || !readQword(info + GAME_INFO_SPEEDRUN_TIME, &bits)) return 0;
    memcpy(seconds, &bits, sizeof(*seconds));
    return 1;
}

/* Every tick, with what is offered now. */
static void keepSpeedrunMark(int offered, uint64_t scene, uint64_t target,
                             int how) {
    if (!offered || how != SKIP_BY_TABLE) return;
    const SceneSkip* listed = sceneSkipFor(scene);
    if (!listed || listed->seconds <= 0.0) return;
    double chapter = 0.0;
    if (!speedrunChapter(&chapter)) return;
    if (target != global_markTarget || chapter + 0.0005 < global_markChapter) {
        global_markTarget = target;
        global_markChapter = chapter;
        global_markSeconds = listed->seconds;
    }
}

/* As a skip's target is played: in speedrun mode, the time the skip leaves
   out added to the chapter's speedrun time. Answers what was added. */
static float speedrunJump(uint64_t target) {
    const uint64_t marked = global_markTarget;
    global_markTarget = 0;
    if (!marked || marked != target) return 0.0f;
    const uint64_t info = gameInfo();
    unsigned int on = 0;
    if (!info || !readDword(info + GAME_INFO_SPEEDRUN_ON, &on) || !on)
        return 0.0f;
    double chapter = 0.0;
    if (!speedrunChapter(&chapter)) return 0.0f;
    const double left = global_markSeconds - (chapter - global_markChapter);
    if (left <= 0.0) return 0.0f;
    const double now = chapter + left;
    uint64_t bits = 0;
    memcpy(&bits, &now, sizeof(bits));
    if (!writeQword(info + GAME_INFO_SPEEDRUN_TIME, bits)) {
        writeLog("SPEEDRUN the speedrun timer could not be given the %.3f s "
                 "the skip left out", left);
        return 0.0f;
    }
    return (float)left;
}

/* ------------------------------------------------------------ the skip */

#define SKIP_TICKS 2   /* ticks between the key and the skip, as the pause menu waits */

static uint64_t global_skipToFunction = 0;   /* GameTaskSubnode::SkipTo */
static int      global_skipKeyWasDown = 0;
static int      global_skipTicks = -1;
static uint64_t global_skipNode = 0;
static uint64_t global_skipTarget = 0;
static uint64_t global_skipCheckpoint = 0;

/* What SceneSkipperDescribe reports of the skips made. */
static uint32_t global_skipsMade = 0;
static uint32_t global_lastHow = 0;
static uint64_t global_lastScene = 0;
static uint64_t global_lastTarget = 0;
static float    global_lastFrame = 0.0f;
static float    global_lastFrom = 0.0f;
static uint32_t global_lastFromPause = 0;   /* the last skip was chosen in the pause menu */
static float    global_lastTimeAdded = 0.0f;  /* what it added to the speedrun timer */

/* A skip to a task waiting its two ticks: how it was found, the scene it was
   made during, and whether it was chosen in the pause menu. */
static uint32_t global_pendingHow = 0;
static uint64_t global_pendingScene = 0;
static uint32_t global_pendingFromPause = 0;

/* The pause menu's skip (below): its offer, worked out by the tick, and Yes
   chosen in its box. */
static volatile LONG global_pauseOffer = 0;
static volatile LONG global_pauseSkipAsked = 0;

static int keyIsDown(int key) {
    unsigned char state = 0;
    if (key <= 0 || key > 0xFF
        || !readByte(global_moduleBase + KEYBOARD_STATE_RVA + (uint64_t)key,
                     &state))
        return 0;
    return (state & 0x80) != 0;
}

/*
    Moves the cinematic playing during scene on to its landing frame, when a
    frame skip names the scene and the cinematic is short of it. 1 when it was
    moved (or the move failed and nothing else should be done), 0 when the key
    is to skip to the scene's target instead.
*/
static int skipToFrame(uint64_t scene) {
    const FrameSkip* landing = frameSkipFor(scene);
    if (!landing) return 0;
    float frame = 0.0f;
    const uint64_t cinematic = frameSkipCinematic(landing, &frame);
    if (!cinematic || frame >= landing->frame - (float)FRAME_SKIP_MARGIN)
        return 0;
    const float seconds = landing->frame / 30.0f;
    if (!stopCinematicAudio(cinematic))
        writeLog("SKIP the audio of %s could not be stopped before the move: "
                 "the call faulted", landing->cinematic);
    int ok = 1;
    GUARD_TRY {
        ((CinematicSeekFn)(global_moduleBase + CINEMATIC_SEEK_RVA))(cinematic,
                                                                    seconds);
    } GUARD_EXCEPT { ok = 0; }
    float pending = 0.0f;
    readFloat(cinematic + CINEMATIC_SEEK_PENDING, &pending);
    if (!ok || pending != seconds)
        writeLog("SKIP %s could not be moved to frame %.0f: %s",
                 landing->cinematic, landing->frame,
                 !ok ? "the call faulted" : "the cinematic did not take the time");
    else {
        ++global_skipsMade;
        global_lastHow = SKIP_BY_MOVE;
        global_lastScene = scene;
        global_lastTarget = 0;
        global_lastFrame = landing->frame;
        global_lastFrom = frame;
        global_lastFromPause = 0;
        global_lastTimeAdded = 0.0f;
    }
    return 1;
}

/* Every tick: the skip three ticks after the key, and the key. */
static void skipWhenAsked(void) {
    if (global_skipTicks > 0) {
        --global_skipTicks;
        return;
    }
    if (global_skipTicks == 0) {
        global_skipTicks = -1;
        unsigned char skipped = 0;
        GUARD_TRY {
            skipped = ((SkipToFn)global_skipToFunction)(global_skipNode,
                                                        global_skipCheckpoint);
        } GUARD_EXCEPT { skipped = 0; }
        if (!skipped) {
            writeLog("SKIP the game refused to play the skip's target task "
                     "(#%016llX) from its checkpoint",
                     (unsigned long long)global_skipTarget);
            global_markTarget = 0;
            return;
        }
        /* A skip the table names may leave timer time out; the game's own
           target is a cinematic's, which the timer does not count. */
        if (global_pendingHow == SKIP_BY_TABLE)
            global_lastTimeAdded = speedrunJump(global_skipTarget);
        else {
            global_markTarget = 0;
            global_lastTimeAdded = 0.0f;
        }
        ++global_skipsMade;
        global_lastHow = global_pendingHow;
        global_lastScene = global_pendingScene;
        global_lastTarget = global_skipTarget;
        global_lastFrame = global_lastFrom = 0.0f;
        global_lastFromPause = global_pendingFromPause;
        return;
    }

    const int down = keyIsDown(global_set.key);
    const int pressed = down && !global_skipKeyWasDown;
    global_skipKeyWasDown = down;
    /* Yes in the pause menu's box, for the skip the pause menu offered for
       the Scene Skipper (pauseConfirmed). */
    const int confirmed = InterlockedExchange(&global_pauseSkipAsked, 0) != 0;
    if (!global_skipToFunction) return;
    if (!confirmed && (!pressed || !keyOn() || inGameMenuUp())) return;

    uint64_t scene = 0, target = 0, checkpoint = 0, countedFrom = 0;
    int cinematic = 0, how = 0;
    if (!resolveSkip(&scene, &target, &checkpoint, &cinematic, &how,
                     &countedFrom))
        return;
    /* The pause menu offered only a skip its table names with no cinematic
       playing; anything else now is left to the game. */
    if (confirmed && (how != SKIP_BY_TABLE || cinematic)) return;

    /* A frame skip for the scene comes first: the cinematic is moved on. */
    if (cinematic && skipToFrame(scene)) return;

    const uint64_t node = nodeNamed(target, 1);
    if (!node) {
        writeLog("SKIP the running task graph has no task #%016llX to skip to",
                 (unsigned long long)target);
        return;
    }
    uint64_t fullName = 0;
    readQword(node + NODE_FULL_NAME, &fullName);
    /* While a cinematic plays the event goes first, as the pause menu sends
       it; with no cinematic it is not sent, which kept the voices of the
       cinematic after a skip out of the study. */
    if (cinematic) {
        BoxedValue skipTo = { fullName, 0, BOXED_NAME };
        BoxedValue atCheckpoint = { 0, 0, BOXED_NAME };
        BoxedValue none = { 0, 0, 0 };
        BoxedValue alsoNone = { 0, 0, 0 };
        int sent = 1;
        GUARD_TRY {
            ((BroadcastFn)(global_moduleBase + BROADCAST_EVENT_RVA))(
                global_moduleBase + STATE_SCRIPT_MANAGER_RVA,
                idOfName("cinematic-skip"), &skipTo, &atCheckpoint, &none,
                &alsoNone);
        } GUARD_EXCEPT { sent = 0; }
        if (!sent) {
            writeLog("SKIP the broadcast of cinematic-skip faulted, so no skip "
                     "is made");
            return;
        }
    }
    global_skipNode = node;
    global_skipTarget = target;
    global_skipCheckpoint = checkpoint ? checkpoint : idOfName("default");
    global_skipTicks = SKIP_TICKS;
    global_pendingHow = (uint32_t)how;
    global_pendingScene = scene;
    global_pendingFromPause = (uint32_t)confirmed;
}

/* ------------------------------------------------- the pause menu's skip */

/*
    The pause menu offers its own Skip Cinematic row during the scenes the
    Scene Skipper skips where no cinematic plays (the study and the car ride),
    and Yes in the box it opens makes the Scene Skipper's skip, the one the
    key makes. During a cinematic the row and its skip stay the game's own.

    The row (optional-skip-cinematic) is drawn when its data source's slot 20
    (01BFE960) finds SkipOffered (0237E93C) true; that call returns to
    PAUSE_ROW_ASKS_RVA. SkipOffered is asked elsewhere too, by the restart
    rows, the menu root's events and scripts, so only that call is answered
    differently: a hook early in SkipOffered (PAUSE_OFFERED_ENTRY_RVA, where
    rax still holds the stack pointer it was entered with, so [rax] is where
    it returns to) notes the thread asking for the row, and a hook where it
    returns (PAUSE_OFFERED_EXIT_RVA, its answer in bl) answers true for that
    thread while the Scene Skipper offers such a skip.

    Yes in the box (confirm_skip_cinematic) reaches the menu root's
    EventHandlerMessageBox (01C85FD8), which asks SkipTarget (0237DB60) for
    the game's own target and does nothing when there is none. A hook just
    after that call (PAUSE_CONFIRMED_RVA, the target in rax) asks the tick for
    the Scene Skipper's skip when the game has none. The tick makes it as the
    key does, sending no cinematic-skip because no cinematic plays, and two
    ticks later plays the target from its checkpoint, as the game plays its
    own target two frames after Yes with the pause menu still up.

    The offer is worked out by the tick, on the game's thread, and only read
    by the hooks. Each hook's bytes are checked before it is placed; where a
    hook cannot be placed, the pause menu offers nothing of the Scene
    Skipper's and the log says so.
*/
#define PAUSE_OFFERED_ENTRY_RVA 0x0237E93Full  /* in SkipOffered: mov [rax+8],rbx */
#define PAUSE_OFFERED_EXIT_RVA  0x0237E9B4ull  /* in SkipOffered: mov rsi,[rsp+48h] */
#define PAUSE_ROW_ASKS_RVA      0x01BFEC84ull  /* where slot 20's call to it returns */
#define PAUSE_CONFIRMED_RVA     0x01C8679Dull  /* in EventHandlerMessageBox: mov r15,rax */

static const uint8_t global_pauseOfferedEntryBytes[] = {
    0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x20 };
static const uint8_t global_pauseOfferedExitBytes[] = { 0x48, 0x8B, 0x74, 0x24, 0x48 };
static const uint8_t global_pauseConfirmedBytes[] = { 0x4C, 0x8B, 0xF8, 0x48, 0x85, 0xC0 };

static volatile LONG global_pauseRowAsker = 0;  /* the thread asking for the row, 0 for none */
static int           global_pauseHooked = 0;    /* all three hooks are in place */

static const char* global_pauseWords = nullptr;   /* the prompt's text for the offer */
static const char* global_pauseAsks = nullptr;    /* the table's question for it, or null */

/* The tick: 1 while a skip the table names is offered and no cinematic
   plays, with the prompt's text for it and the table's question. */
static void updatePauseOffer(void) {
    uint64_t scene = 0, target = 0, checkpoint = 0, countedFrom = 0;
    int cinematic = 0, how = 0;
    const int offered = resolveSkip(&scene, &target, &checkpoint, &cinematic,
                                    &how, &countedFrom)
                     && how == SKIP_BY_TABLE && !cinematic;
    const SceneSkip* listed = offered ? sceneSkipFor(scene) : nullptr;
    global_pauseWords = offered ? labelFor(scene, 0) : nullptr;
    global_pauseAsks = listed ? listed->question : nullptr;
    InterlockedExchange(&global_pauseOffer, offered);
}

/* The tick: what is offered now, for the speedrun timer's mark. */
static void updateSpeedrunMark(void) {
    uint64_t scene = 0, target = 0, checkpoint = 0, countedFrom = 0;
    int cinematic = 0, how = 0;
    const int offered = resolveSkip(&scene, &target, &checkpoint, &cinematic,
                                    &how, &countedFrom);
    keepSpeedrunMark(offered, scene, target, how);
}

/*
    The words of the row and of the box it opens, for the Scene Skipper's own
    offer, in place of the game's:

      - the row and the box's title, the text id MENU_SKIPMOVIE (the game's
        SKIP CINEMATIC): the prompt's text in capitals, SKIP SCENE or SKIP CAR
        RIDE;
      - the box's question, MENU_SKIP_CINEMATIC_CONFIRM: the scene table's
        question where it gives one (Skip the car ride?), else the game's own
        question with its word cinematic replaced by what the prompt names
        (Skip this scene?), in the case the word was written in. A question
        of the game's without the word (another language) is left as it is.

    The game looks both up as the row and the box are built, with TextDb's
    FindText (FIND_TEXT_RVA), which answers from the thirty-two entry cache in
    front of its text database and, on a miss, from the database, adding the
    answer to the cache. Building the pause menu the first time in a session
    looks up more texts than the cache holds, so words put in the cache before
    it had aged out by the time the row looked (test run 14: SKIP CINEMATIC on
    the first opening, SKIP SCENE after). So the answer itself is changed: a
    hook where every lookup of an id ends (TEXT_ANSWERED_RVA, the id in rbx and
    the answer in rax) answers the two ids with the Scene Skipper's words while
    they are on, whatever the cache holds.

    The tick makes the words when the offer starts or its texts change, with
    them off, so the game's own question answers FindText, and turns them on
    once made; when the offer ends it turns them off and the game's words
    answer again. A Skip Cinematic the game offers itself, during a cinematic,
    keeps its words. Each text is written into whichever of its two buffers is
    not answering, so a lookup on another thread never reads it half written.
*/
#define TEXT_DB_RVA          0x0346B8C0ull   /* the text database */
#define FIND_TEXT_RVA        0x0048E750ull   /* TextDb::FindText(text database, id) -> text */
#define TEXT_ANSWERED_RVA    0x0048E87Cull   /* in FindText: mov rdi,[rsp+80h], the answer in rax */
#define PAUSE_TEXT_ROOM      512

static const uint8_t global_textAnsweredBytes[] = {
    0x48, 0x8B, 0xBC, 0x24, 0x80, 0x00, 0x00, 0x00 };

typedef const char* (*FindTextFn)(uint64_t, uint64_t);

/* One text of ours: its id, its two buffers, and the text answering for the
   id while the words are on (null for the game's own). */
struct PauseText {
    const char*          name;
    uint64_t             id;
    char                 buffer[2][PAUSE_TEXT_ROOM];
    int                  next;
    const char* volatile answer;
};

static PauseText     global_pauseTitle = { "MENU_SKIPMOVIE", 0, { { 0 }, { 0 } }, 0,
                                           nullptr };
static PauseText     global_pauseQuestion = { "MENU_SKIP_CINEMATIC_CONFIRM", 0,
                                              { { 0 }, { 0 } }, 0, nullptr };
static volatile LONG global_pauseWordsOn = 0;       /* the words answer their ids */
static int           global_textHooked = 0;         /* the hook in FindText is placed */
static const char*   global_pauseWordsFor = nullptr; /* the prompt's text they were made for */
static const char*   global_pauseAsksFor = nullptr;  /* and the table's question */
static int           global_pauseWordsFailed = 0;    /* FindText faulted: tried no more */

/* Where FindText ends: the Scene Skipper's words for its two ids. */
static void textAnswered(void* registers, void*) {
    if (!global_pauseWordsOn) return;
    const uint64_t id = loaderHookRegister(registers, LOADER_REG_RBX);
    const char* answer = id == global_pauseTitle.id ? global_pauseTitle.answer
                       : id == global_pauseQuestion.id ? global_pauseQuestion.answer
                       : nullptr;
    if (answer)
        loaderHookSetRegister(registers, LOADER_REG_RAX, (uint64_t)(uintptr_t)answer);
}

/* The buffer a text's next words are written into. */
static char* pauseTextBuffer(PauseText* t) {
    return t->buffer[t->next];
}

/* The words just written answer for the text, and the other buffer is the
   next. */
static void pauseTextMade(PauseText* t, int made) {
    t->answer = made ? t->buffer[t->next] : nullptr;
    if (made) t->next ^= 1;
}

/* What the prompt's text names, after its Skip: "Skip Car Ride" names "car
   ride". */
static void promptNoun(const char* label, char* out, size_t room) {
    const char* at = label;
    if (_strnicmp(at, "Skip ", 5) == 0) at += 5;
    size_t n = 0;
    for (; *at && n + 1 < room; ++at)
        out[n++] = (char)tolower((unsigned char)*at);
    out[n] = 0;
}

/*
    text with each whole word cinematic, in any case, replaced by noun in the
    case the word was written in (CINEMATIC, Cinematic or cinematic). 0 when
    the word is not there or the result does not fit.
*/
static int replaceCinematic(const char* text, const char* noun, char* out,
                            size_t room) {
    static const char word[] = "cinematic";
    const size_t length = sizeof(word) - 1;
    size_t n = 0;
    int replaced = 0;
    for (const char* at = text; *at; ) {
        const int starts = at == text || !isalpha((unsigned char)at[-1]);
        if (starts && _strnicmp(at, word, length) == 0
            && !isalpha((unsigned char)at[length])) {
            const int allUpper = isupper((unsigned char)at[0])
                              && isupper((unsigned char)at[1]);
            const int firstUpper = isupper((unsigned char)at[0]);
            for (size_t k = 0; noun[k]; ++k) {
                if (n + 1 >= room) return 0;
                char c = noun[k];
                if (allUpper || (firstUpper && k == 0))
                    c = (char)toupper((unsigned char)c);
                out[n++] = c;
            }
            at += length;
            replaced = 1;
            continue;
        }
        if (n + 1 >= room) return 0;
        out[n++] = *at++;
    }
    out[n] = 0;
    return replaced;
}

/* The words made for the prompt's text label, with the words off: the title
   in capitals, and the table's question, or the game's question with the
   prompt's noun in it. */
static void makePauseWords(const char* label, const char* question) {
    char* title = pauseTextBuffer(&global_pauseTitle);
    size_t n = 0;
    for (const char* at = label; *at && n + 1 < PAUSE_TEXT_ROOM; ++at)
        title[n++] = (char)toupper((unsigned char)*at);
    title[n] = 0;
    pauseTextMade(&global_pauseTitle, n > 0);

    char* asked = pauseTextBuffer(&global_pauseQuestion);
    if (question && question[0]) {
        strncpy_s(asked, PAUSE_TEXT_ROOM, question, _TRUNCATE);
        pauseTextMade(&global_pauseQuestion, 1);
        return;
    }

    /* The game's own question; the answer points into the cache's pool and is
       copied at once. After a fault it is not asked for again. */
    if (global_pauseWordsFailed) {
        pauseTextMade(&global_pauseQuestion, 0);
        return;
    }
    char game[PAUSE_TEXT_ROOM];
    game[0] = 0;
    int ok = 1;
    GUARD_TRY {
        const char* found = ((FindTextFn)(global_moduleBase + FIND_TEXT_RVA))(
            global_moduleBase + TEXT_DB_RVA, global_pauseQuestion.id);
        if (found) strncpy_s(game, sizeof(game), found, _TRUNCATE);
    } GUARD_EXCEPT { ok = 0; }
    if (!ok) {
        global_pauseWordsFailed = 1;
        writeLog("PAUSE asking for the game's question for the skip box faulted, "
                 "so the box keeps the game's question");
        pauseTextMade(&global_pauseQuestion, 0);
        return;
    }
    char noun[64];
    promptNoun(label, noun, sizeof(noun));
    pauseTextMade(&global_pauseQuestion,
                  game[0] && noun[0]
                  && replaceCinematic(game, noun, asked, PAUSE_TEXT_ROOM));
}

/* Whether two texts, either of which may be null, are the same. */
static int sameWords(const char* a, const char* b) {
    if (!a || !b) return a == b;
    return a == b || strcmp(a, b) == 0;
}

/* Every tick: the words on while the offer holds, made again when its texts
   change, and off once it has ended. */
static void keepPauseWords(void) {
    if (!global_pauseHooked || !global_textHooked) return;
    const char* wanted = global_pauseOffer ? global_pauseWords : nullptr;
    const char* asks = wanted ? global_pauseAsks : nullptr;
    if (!wanted) {
        InterlockedExchange(&global_pauseWordsOn, 0);
        global_pauseWordsFor = nullptr;
        global_pauseAsksFor = nullptr;
        return;
    }
    if (sameWords(wanted, global_pauseWordsFor)
        && sameWords(asks, global_pauseAsksFor))
        return;
    InterlockedExchange(&global_pauseWordsOn, 0);
    makePauseWords(wanted, asks);
    global_pauseWordsFor = wanted;
    global_pauseAsksFor = asks;
    InterlockedExchange(&global_pauseWordsOn, 1);
}

/* Early in SkipOffered: the thread is asking for the row, or it is not. */
static void pauseOfferedEntry(void* registers, void*) {
    uint64_t returnTo = 0;
    if (!readQword(loaderHookRegister(registers, LOADER_REG_RAX), &returnTo))
        return;
    const LONG me = (LONG)GetCurrentThreadId();
    if (returnTo == global_moduleBase + PAUSE_ROW_ASKS_RVA)
        InterlockedExchange(&global_pauseRowAsker, me);
    else
        InterlockedCompareExchange(&global_pauseRowAsker, 0, me);
}

/* Where SkipOffered returns: true for the row while the offer holds. */
static void pauseOfferedExit(void* registers, void*) {
    const LONG me = (LONG)GetCurrentThreadId();
    if (InterlockedCompareExchange(&global_pauseRowAsker, 0, me) != me) return;
    if (!global_pauseOffer) return;
    const uint64_t answer = loaderHookRegister(registers, LOADER_REG_RBX);
    if (!(answer & 0xFF))
        loaderHookSetRegister(registers, LOADER_REG_RBX, (answer & ~0xFFull) | 1);
}

/* Yes in the box: the game's own target, or the Scene Skipper's skip. */
static void pauseConfirmed(void* registers, void*) {
    if (loaderHookRegister(registers, LOADER_REG_RAX)) return;
    if (global_pauseOffer) InterlockedExchange(&global_pauseSkipAsked, 1);
}

/* One of the hooks, placed where the game's bytes are those expected;
   without says what is lost when it cannot be. */
static int hookPauseMenu(const char* name, uint64_t rva, const uint8_t* bytes,
                         size_t count, LoaderHookCallback callback,
                         const char* without) {
    int same = 0;
    GUARD_TRY {
        same = memcmp((const void*)(uintptr_t)(global_moduleBase + rva), bytes,
                      count) == 0;
    } GUARD_EXCEPT { same = 0; }
    if (!same) {
        writeLog("PAUSE the game's code at %08llX is not what the Scene Skipper "
                 "expects, so %s", (unsigned long long)rva, without);
        return 0;
    }
    if (!loaderHookAddress(name, global_moduleBase + rva, callback, nullptr)) {
        writeLog("PAUSE the loader refused the hook %s, so %s", name, without);
        return 0;
    }
    return 1;
}

/* The hooks, Yes's first and the row's entry last, so a row is only ever
   offered with everything in place to make its skip; then the words. */
static void installPauseMenuSkip(void) {
    static const char* const noSkip = "the pause menu offers no skip of its own";
    if (!global_skipToFunction) return;
    if (!hookPauseMenu("sceneskipper_pause_confirmed", PAUSE_CONFIRMED_RVA,
                       global_pauseConfirmedBytes,
                       sizeof(global_pauseConfirmedBytes), pauseConfirmed, noSkip))
        return;
    if (!hookPauseMenu("sceneskipper_pause_offered_exit", PAUSE_OFFERED_EXIT_RVA,
                       global_pauseOfferedExitBytes,
                       sizeof(global_pauseOfferedExitBytes), pauseOfferedExit,
                       noSkip))
        return;
    global_pauseHooked = hookPauseMenu("sceneskipper_pause_offered_entry",
                                       PAUSE_OFFERED_ENTRY_RVA,
                                       global_pauseOfferedEntryBytes,
                                       sizeof(global_pauseOfferedEntryBytes),
                                       pauseOfferedEntry, noSkip);
    if (!global_pauseHooked) return;
    global_pauseTitle.id = idOfName(global_pauseTitle.name);
    global_pauseQuestion.id = idOfName(global_pauseQuestion.name);
    global_textHooked = hookPauseMenu("sceneskipper_text_answered",
                                      TEXT_ANSWERED_RVA,
                                      global_textAnsweredBytes,
                                      sizeof(global_textAnsweredBytes),
                                      textAnswered,
                                      "the pause menu's skip keeps the game's "
                                      "words");
}

/* ------------------------------------------------------------ the prompt */

#define GET_OR_CREATE_CHILD_RVA  0x0254A1D8ull   /* the widget system's get-or-create */
#define ASSIGN_CATEGORY_RVA      0x025450FCull   /* (widget, its own categories) */
#define KEY_ICON_TEXT_RVA        0x020E0EE4ull   /* writes a key's icon as text into a builder */
#define ICON_BUILDER_TYPE_RVA    0x02C79208ull   /* that builder's class, as 020E1020 makes it */
#define FIRST_CHILD_OBJECT_RVA   0x02549FC0ull   /* (container) -> its first child, of any kind */
#define NEXT_SIBLING_OBJECT_RVA  0x0254A190ull   /* (child) -> the next */
#define WIDGET_FILE_ID           0x1A8           /* the id of the widget file a widget was made from */
#define WIDGET_OWN_CATEGORIES    0x2E8
#define WIDGET_IS_VISIBLE_SLOT   0x320           /* IsVisible */
#define WIDGET_SET_PROPERTY_SLOT 0x380           /* the property setter */
#define LAYOUT_RECT              0x248           /* the rectangle laid out: left, top, width, height */
#define TEXT_CLASS_RVA           0x02EB83D0u     /* a Text's class table */
#define IMAGE_CLASS_RVA          0x02EB7F00u     /* an Image's class table */
#define TITLE_CARDS_BIT          (1u << 21)      /* the category title-cards */
#define PROMPT_FILE_PREFIX       "scene-skipper/skip-prompt-"
#define PROMPT_NAME              "ss-scene-skip-prompt"
#define PROMPT_DOCUMENT_ROOM     8192
#define PROMPT_REGISTER_TRIES    4       /* registrar calls a file is tried in */
#define REGISTRAR_HOOK_NAME      "WidgetFile_Register"   /* the Mod Menu Framework's
                                            name for the same hook: the loader
                                            finds a trampoline by the name a
                                            hook was first installed under */
#define PROMPT_FADE_IN_MS        500
#define PROMPT_FADE_OUT_MS       800
#define PROMPT_FAST_MS           2000    /* checked every tick this long after a make or a find */
#define PROMPT_CHECK_MS          250
#define PROMPT_MAKE_GAP_MS       500     /* the least time between makes */
#define PROMPT_MAKE_GAP_MAX_MS   30000
#define PROMPT_MAX_FADE_STEP_MS  100     /* the most one tick moves a fade */
#define PROMPT_OFFER_GRACE_MS    1500    /* an offer back within this is the same one */
#define PROMPT_MEASURE_CHECKS    40      /* checks the probe is read in */
#define PROMPT_MAX_CHILDREN      4096
#define PROMPT_MARGIN_X          0.03f   /* of the interface's width */
#define PROMPT_MARGIN_Y          0.05f   /* of its height */
#define READER_SEARCH_BACK       0x8000
#define MAX_READER_CALLERS       8

#define PHASE_HIDDEN             0       /* no skip is offered */
#define PHASE_IN                 1       /* fading in */
#define PHASE_HELD               2       /* at its opacity */
#define PHASE_OUT                3       /* fading out */
#define PHASE_GONE               4       /* faded out; the key still skips */

typedef uint64_t (*WidgetStepFn)(uint64_t);
typedef uint64_t (*GetOrCreateChildFn)(uint64_t, const void*, const char*,
                                       const char*, uint64_t, uint64_t,
                                       uint64_t, uint64_t);
typedef void (*AssignCategoryFn)(uint64_t, unsigned);
typedef unsigned char (*WidgetAnswerFn)(uint64_t);
typedef void (*SetPropertyFn)(uint64_t, uint64_t, const BoxedValue*);
/* The text builder 020E1020 makes on its stack for 020E0EE4: its class, its
   capacity (256), the length written, and the text. */
struct IconBuilder {
    uint64_t type;
    uint64_t capacity;
    uint64_t length;
    char     text[0x108];
};
/* 020E0EE4(the game's code for the key, the builder, large): large asks for
   html/pc/<name>_large.svg first, falling back to <name>.svg. */
typedef void (*KeyIconTextFn)(uint64_t, IconBuilder*, uint64_t);
/* jdom::ParseData(the slot the document is written to, the text, its length,
   the allocator: null for the engine's current memory scope). */
typedef void (*ParseDocumentFn)(void*, const char*, uint64_t, void*);
/* The registrar: (the file table, the name, the document). */
typedef void (*RegisterWidgetFileFn)(void*, const char*, void*);

/* One of the prompt's widget files: its name, its JSON and whether the
   registrar has it. */
struct PromptFile {
    int           size;
    char          name[64];
    uint64_t      id;
    char          text[PROMPT_DOCUMENT_ROOM];
    int           length;
    volatile LONG registered;
    LONG          tries;
};

static PromptFile global_promptFiles[PROMPT_SIZES];
static PromptFile* volatile global_promptFileUsed = nullptr;  /* the file for the size */
static volatile LONG global_promptFilesDone = 0;  /* registered, or given up on */
static volatile LONG global_promptRegisterBusy = 0;
static int       global_promptFilesSaid = 0;
static uint64_t  global_parseDocument = 0;     /* jdom::ParseData */
static uint64_t  global_registerWidgetFile = 0;/* the registrar's trampoline */
static uint64_t  global_promptParseSlot[16];   /* where ParseData writes */
static int       global_promptMakeFailed = 0;  /* a make faulted or answered nothing */
static uint64_t  global_promptLive = 0;        /* the prompt the last check found */
static uint64_t  global_promptImage = 0;       /* its icon */
static uint64_t  global_promptLabel = 0;       /* its text */
static uint64_t  global_promptProbe = 0;       /* its probe */
static ULONGLONG global_promptLastMake = 0;
static unsigned  global_promptMakeGap = PROMPT_MAKE_GAP_MS;
static ULONGLONG global_promptNextCheck = 0;
static ULONGLONG global_promptFastUntil = 0;   /* checked every tick until then */
static char      global_promptTexts[4][256];   /* the texts set, kept alive */
static int       global_promptTextNext = 0;
static const char* global_promptTextNow = "";  /* the text last set */
static char      global_promptIcon[256];       /* what 020E0EE4 gives for the skip key */
static char      global_promptIconLarge[256];  /* and for its large picture */
static int       global_promptIconKey = -1;    /* the key they were asked for */
static int       global_promptIconIsPath = 0;
static int       global_promptIconMeasured = 0;/* the icon's box sized from the probe */
static int       global_promptIconChecks = 0;
static int       global_promptPhase = PHASE_HIDDEN;
static ULONGLONG global_promptPhaseMs = 0;     /* time drawn in this phase */
static ULONGLONG global_promptLastTick = 0;    /* when the fade last ran */
static uint64_t  global_promptOffer = 0;       /* the offer shown: its target and text */
static uint64_t  global_promptLostOffer = 0;   /* the offer last hidden, */
static ULONGLONG global_promptLostAt = 0;      /* when, */
static int       global_promptLostPhase = PHASE_HIDDEN;  /* and in which phase */
static float     global_promptAlpha = -1.0f;   /* the opacity last set; -1 not yet */

/* ---------------------------------------------- the prompt's widget files */

/*
    One prompt widget file, as JSON, for a size. Its numbers are the game's
    button prompt's at 100% times the size: 64 high, the icon's box 54 wide
    until the probe gives the picture's own size, 15 between the icon and the
    text, the text at 41 points. Parts:

      root   a Group, no engine class, laying its children out across, not
             focusable, and hidden (visible 0) until it is shown, so an
             instance nobody has filled shows nothing
      icon   an Image drawn StretchAspect in a box sized here, as high as the
             root from its anchors
      label  a Text in the look of the game's prompts (SoftOutline, the
             Default face, white), as high as the root from its anchors; no
             style, since a Text's style sets its size over pointSize
      probe  an Image drawn at the picture's own size (Original, autoSize),
             invisible (color.a 0) and outside the layout (layoutMode
             Exclude): it is given the key's icon so that size can be read

    A float is written with a decimal point, since the engine reads a whole
    number as an integer and then finds no float where it wants one. Each
    item has a guid of its own; the last three digits are the size in
    hexadecimal, so no two files share one.
*/
#define PROMPT_GUID_ROOT   "46a9c1612b4d4e6798314c48f9142"
#define PROMPT_GUID_ICON   "3f1fe674d84540b1aa9dcd70eaec3"
#define PROMPT_GUID_LABEL  "a3595acff28e4ff192eb978d07883"
#define PROMPT_GUID_PROBE  "a6cdb856b69e47029461ea018746d"

static const char global_promptDocumentFormat[] =
    "{\"size\": [%.4f, %.4f], \"grid\": [100, 100], \"lang\": \"eng\", "
    "\"defaultEngineClass\": \"\", \"defaultScript\": \"\", \"fps\": 60.0, "
    "\"currentFrame\": 0.0, \"textTracking.x\": 0.0, \"textTracking.y\": 0.0, "
    "\"animation\": {\"labels\": []}, \"items\": ["
    /* root */
    "{\"animation\": {\"keyframes\": {}}, \"anchorMaxN.x\": 0.03, "
    "\"anchorMaxN.y\": 0.95, \"anchorMinN.x\": 0.03, \"anchorMinN.y\": 0.95, "
    "\"class\": \"Group\", \"color.a\": 255, \"depth\": 0.0, "
    "\"flowDirection\": \"Horizontal\", \"flowGridSize.x\": 0, "
    "\"flowGridSize.y\": 0, \"flowMargins.b\": 0.0, \"flowMargins.l\": 0.0, "
    "\"flowMargins.r\": 0.0, \"flowMargins.t\": 0.0, \"flowSpacing\": %.4f, "
    "\"focusable\": 0, \"guid\": \"" PROMPT_GUID_ROOT "%03x\", "
    "\"inheritAlpha\": 1, \"inheritRotation\": 1, \"inheritScale\": 1, "
    "\"inheritTint\": 0, \"isHoverable\": 0, \"isMaskInverted\": 0, "
    "\"isMaskOrClip\": 0, \"layoutMode\": \"Unspecified\", \"locked\": 0, "
    "\"name\": \"root\", \"noSpawn\": 0, \"parent\": null, \"pivotAs.x\": 0.0, "
    "\"pivotAs.y\": 0.0, \"pivotN.x\": 0.0, \"pivotN.y\": 1.0, "
    "\"rectSizeDelta.height\": %.4f, \"rectSizeDelta.width\": %.4f, "
    "\"resizeDirection\": \"AllDirections\", \"rotation\": 0.0, "
    "\"scale.x\": 1.0, \"scale.y\": 1.0, \"tint.blend\": 0.0, "
    "\"tint.end\": 4294967295, \"tint.start\": 4294967295, \"visible\": 0}, "
    /* icon */
    "{\"animation\": {\"keyframes\": {}}, \"alignH\": 2, \"alignV\": 1, "
    "\"anchorMaxN.x\": 0.0, \"anchorMaxN.y\": 1.0, \"anchorMinN.x\": 0.0, "
    "\"anchorMinN.y\": 0.0, \"autoSize\": 0, "
    "\"brightnessAdjustId\": \"inspect-prompt-brightness\", "
    "\"brightnessSettingsId\": \"button-mash-rect-size\", \"class\": \"Image\", "
    "\"color.a\": 255, \"depth\": 0.0, \"effect\": \"SoftOutline\", "
    "\"explicitSize\": 0, \"focusable\": 0, "
    "\"guid\": \"" PROMPT_GUID_ICON "%03x\", \"inheritAlpha\": 1, "
    "\"inheritRotation\": 1, \"inheritScale\": 1, \"inheritTint\": 0, "
    "\"isHoverable\": 0, \"isMaskInverted\": 0, \"isMaskOrClip\": 0, "
    "\"layoutMode\": \"Unspecified\", \"locked\": 0, \"name\": \"icon\", "
    "\"noSpawn\": 0, \"parent\": \"" PROMPT_GUID_ROOT "%03x\", \"path\": \"\", "
    "\"pivotAs.x\": %.4f, \"pivotAs.y\": 0.0, \"pivotN.x\": 0.5, "
    "\"pivotN.y\": 0.5, \"rasterSize.height\": 0.0, \"rasterSize.width\": 0.0, "
    "\"rectSizeDelta.height\": 0.0, \"rectSizeDelta.width\": %.4f, "
    "\"rotation\": 0.0, \"scale.x\": 1.0, \"scale.y\": 1.0, "
    "\"scaleMode\": \"StretchAspect\", "
    "\"slices\": [0.328125, 0.328125, 0.328125, 0.328125], "
    "\"tint.blend\": 0.0, "
    "\"tint.end\": \"t2-white\", \"tint.start\": \"t2-white\", \"visible\": 1}, "
    /* label */
    "{\"animation\": {\"keyframes\": {}}, \"alignH\": 0, \"alignV\": 2, "
    "\"anchorMaxN.x\": 0.0, \"anchorMaxN.y\": 1.0, \"anchorMinN.x\": 0.0, "
    "\"anchorMinN.y\": 0.0, \"autoSize\": 1, \"autoSizeMode\": \"Horizontal\", "
    "\"brightnessAdjustId\": \"inspect-prompt-brightness\", "
    "\"brightnessSettingsId\": \"drop-shadow-rect-size\", \"cipherId\": \"\", "
    "\"class\": \"Text\", \"color.a\": 255, \"depth\": 1.0, "
    "\"effect\": \"SoftOutline\", \"faceId\": \"Default\", \"focusable\": 0, "
    "\"guid\": \"" PROMPT_GUID_LABEL "%03x\", \"inheritAlpha\": 1, "
    "\"inheritRotation\": 1, \"inheritScale\": 1, \"inheritTint\": 0, "
    "\"isHoverable\": 0, \"isMaskInverted\": 0, \"isMaskOrClip\": 0, "
    "\"layoutMode\": \"Unspecified\", \"locked\": 0, \"multiline\": 0, "
    "\"name\": \"label\", \"noSpawn\": 0, "
    "\"parent\": \"" PROMPT_GUID_ROOT "%03x\", \"pivotAs.x\": %.4f, "
    "\"pivotAs.y\": 0.0, \"pivotN.x\": 0.0, \"pivotN.y\": 0.5, "
    "\"pointSize\": %d, \"rectSizeDelta.height\": 0.0, "
    "\"rectSizeDelta.width\": %.4f, \"rotation\": 0.0, \"scale.x\": 1.0, "
    "\"scale.y\": 1.0, \"text\": \"\", "
    "\"tint.blend\": 0.0, \"tint.end\": \"t2-white\", "
    "\"tint.start\": \"t2-white\", \"tracking.x\": 0.0, \"tracking.y\": 0.0, "
    "\"visible\": 1}, "
    /* probe */
    "{\"animation\": {\"keyframes\": {}}, \"alignH\": 2, \"alignV\": 1, "
    "\"anchorMaxN.x\": 0.0, \"anchorMaxN.y\": 0.5, \"anchorMinN.x\": 0.0, "
    "\"anchorMinN.y\": 0.5, \"autoSize\": 1, \"class\": \"Image\", "
    "\"color.a\": 0, \"depth\": 2.0, \"explicitSize\": 0, \"focusable\": 0, "
    "\"guid\": \"" PROMPT_GUID_PROBE "%03x\", \"inheritAlpha\": 1, "
    "\"inheritRotation\": 1, \"inheritScale\": 1, \"inheritTint\": 0, "
    "\"isHoverable\": 0, \"isMaskInverted\": 0, \"isMaskOrClip\": 0, "
    "\"layoutMode\": \"Exclude\", \"locked\": 0, \"name\": \"probe\", "
    "\"noSpawn\": 0, \"parent\": \"" PROMPT_GUID_ROOT "%03x\", \"path\": \"\", "
    "\"pivotAs.x\": 0.0, \"pivotAs.y\": 0.0, \"pivotN.x\": 0.0, "
    "\"pivotN.y\": 0.5, \"rasterSize.height\": 0.0, \"rasterSize.width\": 0.0, "
    "\"rectSizeDelta.height\": 0.0, \"rectSizeDelta.width\": 0.0, "
    "\"rotation\": 0.0, \"scale.x\": 1.0, \"scale.y\": 1.0, "
    "\"scaleMode\": \"Original\", "
    "\"slices\": [0.328125, 0.328125, 0.328125, 0.328125], "
    "\"tint.blend\": 0.0, \"tint.end\": 4294967295, "
    "\"tint.start\": 4294967295, \"visible\": 1}"
    "]}";

/* Writes the JSON of the prompt's widget file for a size; its length, or 0
   when it does not fit. */
static int writePromptDocument(char* out, size_t room, int size) {
    const double f = (double)size / 100.0;
    const double height = 64.0 * f, width = 600.0 * f;
    const double iconWidth = 54.0 * f;
    const double spacing = 15.0 * f;
    const int points = (int)(41.0 * f + 0.5);
    const unsigned tail = (unsigned)size & 0xFFFu;
    const int written = _snprintf_s(out, room, _TRUNCATE,
        global_promptDocumentFormat,
        width, height,                                   /* the file's size */
        spacing, tail, height, width,                    /* root */
        tail, tail, iconWidth / 2.0, iconWidth,          /* icon */
        tail, tail, iconWidth + spacing, points, 400.0 * f, /* label */
        tail, tail);                                     /* probe */
    return written > 0 ? written : 0;
}

/* The prompt's widget file for a size in steps of PROMPT_SIZE_STEP, or null. */
static PromptFile* promptFileFor(int size) {
    for (int i = 0; i < PROMPT_SIZES; ++i)
        if (global_promptFiles[i].size == size) return &global_promptFiles[i];
    return nullptr;
}

/*
    The widget file reader: the one function that turns a document into a
    widget file. It carries no signature of its own; Gui2::WidgetFile::
    LoadAnimations does, and the reader is the only function that calls it.
    Walking back from that call to the int3 padding in front of it gives the
    reader's first byte.
*/

/* The only rel32 call to target, or 0 when there is not exactly one. */
static uint64_t soleCallSite(uint64_t target) {
    if (!global_textBase || !global_textSize || !target) return 0;
    const uint8_t* text = (const uint8_t*)global_textBase;
    uint64_t found = 0;
    int count = 0;
    for (uint64_t i = 0; i + 5 <= global_textSize; ++i) {
        if (text[i] != 0xE8) continue;
        int32_t relative = 0;
        memcpy(&relative, text + i + 1, 4);
        const uint64_t site = global_textBase + i;
        if ((uint64_t)((int64_t)site + 5 + relative) != target) continue;
        if (++count > 1) return 0;
        found = site;
    }
    return count == 1 ? found : 0;
}

/* The start of the function holding site: the byte after the nearest run of
   int3 padding in front of it. */
static uint64_t functionStartBefore(uint64_t site) {
    for (uint64_t at = site; at > global_textBase + 2; --at) {
        unsigned char one = 0, two = 0;
        if (!readByte(at - 1, &one) || !readByte(at - 2, &two)) return 0;
        if (one == 0xCC && two == 0xCC) return at;
        if (site - at > READER_SEARCH_BACK) return 0;
    }
    return 0;
}

static uint64_t findWidgetFileReader(void) {
    const uint64_t animations = loaderFindFunction(
        "Gui2::WidgetFile::LoadAnimations", loadAnimationsSignature);
    const uint64_t site = animations ? soleCallSite(animations) : 0;
    return site ? functionStartBefore(site) : 0;
}

/*
    The widget file registrar (0243A180 in 1.1.5.0), find or create:
    registrar(the file table, the name, the document). Exactly two functions
    call the reader, and the registrar is the one that calls it twice, once on
    each of its branches.
*/
static uint64_t findWidgetFileRegistrar(uint64_t reader) {
    if (!global_textBase || !global_textSize || !reader) return 0;
    uint64_t starts[MAX_READER_CALLERS];
    int counts[MAX_READER_CALLERS], found = 0;
    const uint8_t* text = (const uint8_t*)global_textBase;
    for (uint64_t i = 0; i + 5 <= global_textSize; ++i) {
        if (text[i] != 0xE8) continue;
        int32_t relative = 0;
        memcpy(&relative, text + i + 1, 4);
        const uint64_t site = global_textBase + i;
        if ((uint64_t)((int64_t)site + 5 + relative) != reader) continue;
        const uint64_t start = functionStartBefore(site);
        if (!start) continue;
        int at = 0;
        while (at < found && starts[at] != start) ++at;
        if (at == found) {
            if (found >= MAX_READER_CALLERS) continue;
            starts[found] = start;
            counts[found++] = 0;
        }
        ++counts[at];
    }
    int chosen = -1, twice = 0;
    for (int k = 0; k < found; ++k)
        if (counts[k] >= 2) { chosen = k; ++twice; }
    return twice == 1 ? starts[chosen] : 0;
}

/*
    The engine's current memory scope, gs:[0x20] -> [0] -> [+0x38]. ParseData
    allocates through it when handed a null allocator, and faults when it is
    null. It is live inside the registrar.
*/
static uint64_t engineMemoryScope(void) {
    const uint64_t block = __readgsqword(0x20);
    uint64_t first = 0, scope = 0;
    if (!block || !readQword(block, &first) || !first) return 0;
    if (!readQword(first + 0x38, &scope)) return 0;
    return scope;
}

/* Puts the next of the prompt's files not yet in the table into it, the one
   in use first. Runs at a registrar call, from inside it; table is that
   call's first argument. */
static void registerNextPromptFile(uint64_t table) {
    PromptFile* file = nullptr;
    PromptFile* used = global_promptFileUsed;
    if (used && !used->registered && used->tries < PROMPT_REGISTER_TRIES)
        file = used;
    for (int i = 0; i < PROMPT_SIZES && !file; ++i)
        if (!global_promptFiles[i].registered
            && global_promptFiles[i].tries < PROMPT_REGISTER_TRIES)
            file = &global_promptFiles[i];
    if (!file) return;
    ++file->tries;

    if (!engineMemoryScope()) return;   /* tried again at the next call */
    if (!global_registerWidgetFile)
        global_registerWidgetFile = loaderHookTrampoline(REGISTRAR_HOOK_NAME);
    if (!global_registerWidgetFile) {
        file->tries = PROMPT_REGISTER_TRIES;
        return;
    }

    memset(global_promptParseSlot, 0, sizeof(global_promptParseSlot));
    int ok = 1;
    GUARD_TRY {
        ((ParseDocumentFn)global_parseDocument)(global_promptParseSlot,
                                                file->text,
                                                (uint64_t)file->length, nullptr);
    } GUARD_EXCEPT { ok = 0; }
    const uint64_t document = global_promptParseSlot[0];
    if (!ok || !document) {
        if (file->tries >= PROMPT_REGISTER_TRIES)
            writeLog("PROMPT the widget file %s could not be parsed (%s)",
                     file->name, ok ? "it parsed to nothing"
                                    : "jdom::ParseData faulted");
        return;
    }
    ok = 1;
    GUARD_TRY {
        ((RegisterWidgetFileFn)global_registerWidgetFile)(
            (void*)table, file->name, (void*)document);
    } GUARD_EXCEPT { ok = 0; }
    if (!ok) {
        if (file->tries >= PROMPT_REGISTER_TRIES)
            writeLog("PROMPT the widget file %s faulted in the registrar",
                     file->name);
        return;
    }
    InterlockedExchange(&file->registered, 1);
}

/* At every registrar call while any of the prompt's files waits: one file. */
static void promptRegistrarHook(void* registers, void* user) {
    (void)user;
    if (global_promptFilesDone) return;
    if (InterlockedCompareExchange(&global_promptRegisterBusy, 1, 0) != 0)
        return;
    registerNextPromptFile(loaderHookRegister(registers, LOADER_REG_RCX));
    int waiting = 0;
    for (int i = 0; i < PROMPT_SIZES; ++i)
        if (!global_promptFiles[i].registered
            && global_promptFiles[i].tries < PROMPT_REGISTER_TRIES)
            ++waiting;
    if (!waiting) InterlockedExchange(&global_promptFilesDone, 1);
    InterlockedExchange(&global_promptRegisterBusy, 0);
}

/*
    Writes the prompt's widget files and hooks the registrar, so they are put
    in the game's table during its widget file load, a few seconds after the
    game starts. Without them no prompt is made; the key still skips.
*/
static void installPromptFiles(void) {
    for (int i = 0; i < PROMPT_SIZES; ++i) {
        PromptFile* file = &global_promptFiles[i];
        file->size = PROMPT_SIZE_LEAST + i * PROMPT_SIZE_STEP;
        sprintf_s(file->name, "%s%d", PROMPT_FILE_PREFIX, file->size);
        file->id = idOfName(file->name);
        file->length = writePromptDocument(file->text, sizeof(file->text),
                                           file->size);
        file->registered = 0;
        file->tries = file->length ? 0 : PROMPT_REGISTER_TRIES;
        if (!file->length)
            writeLog("PROMPT the widget file %s does not fit in %d bytes",
                     file->name, PROMPT_DOCUMENT_ROOM);
    }
    global_promptFileUsed = promptFileFor(global_set.size);

    global_parseDocument = loaderFindFunction("jdom::ParseData",
                                              parseDataSignature);
    const uint64_t reader = global_parseDocument ? findWidgetFileReader() : 0;
    const uint64_t registrar = reader ? findWidgetFileRegistrar(reader) : 0;
    if (!global_parseDocument || !registrar
        || !loaderHookAddress(REGISTRAR_HOOK_NAME, registrar,
                              promptRegistrarHook, nullptr)) {
        writeLog("PROMPT %s, so the prompt's widget files cannot be registered "
                 "and no prompt is shown; the skip key still skips",
                 !global_parseDocument ? "jdom::ParseData was not found"
                 : !reader ? "the widget file reader was not found"
                 : !registrar ? "the widget file registrar could not be told "
                                "apart from the reader's other caller"
                 : "the loader refused the hook on the registrar");
        global_promptFilesDone = 1;
        for (int i = 0; i < PROMPT_SIZES; ++i)
            global_promptFiles[i].tries = PROMPT_REGISTER_TRIES;
        return;
    }
    global_registerWidgetFile = loaderHookTrampoline(REGISTRAR_HOOK_NAME);
}

/* ------------------------------------------------- the prompt's widgets */

static uint64_t firstChildObjectOf(uint64_t container) {
    if (container < SMALLEST_POINTER) return 0;
    uint64_t answer = 0;
    GUARD_TRY {
        answer = ((WidgetStepFn)(global_moduleBase
                                 + FIRST_CHILD_OBJECT_RVA))(container);
    } GUARD_EXCEPT { answer = 0; }
    return answer;
}

static uint64_t nextSiblingObjectOf(uint64_t child) {
    if (child < SMALLEST_POINTER) return 0;
    uint64_t answer = 0;
    GUARD_TRY {
        answer = ((WidgetStepFn)(global_moduleBase
                                 + NEXT_SIBLING_OBJECT_RVA))(child);
    } GUARD_EXCEPT { answer = 0; }
    return answer;
}

/* The interface's root widget, or 0. */
static uint64_t hudRoot(void) {
    return objectOfHandle(global_moduleBase + HUD_HANDLE_RVA);
}

/* The method in a widget's table at slot, or 0. */
static uint64_t methodOf(uint64_t widget, unsigned slot) {
    uint64_t table = 0, m = 0;
    if (!readQword(widget, &table) || !table) return 0;
    if (!readQword(table + slot, &m) || !inCode(m)) return 0;
    return m;
}

/* The RVA of a widget's class table, or 0. */
static unsigned tableRva(uint64_t widget) {
    uint64_t table = 0;
    if (!readQword(widget, &table) || !table || !inModule(table)) return 0;
    return (unsigned)(table - global_moduleBase);
}

/* Whether the engine would draw a widget: 1, 0, or -1 when it cannot be
   asked. */
static int widgetDrawn(uint64_t widget) {
    const uint64_t method = methodOf(widget, WIDGET_IS_VISIBLE_SLOT);
    if (!method) return -1;
    int answer = -1;
    GUARD_TRY { answer = ((WidgetAnswerFn)method)(widget) ? 1 : 0; }
    GUARD_EXCEPT { answer = -1; }
    return answer;
}

/* The widget file a widget was made from, or 0. */
static uint64_t widgetFileOf(uint64_t widget) {
    uint64_t file = 0;
    readQword(widget + WIDGET_FILE_ID, &file);
    return file;
}

/*
    The one widget directly under parent made from file; 0 when there is none,
    and 0 with *several set when there is more than one.
*/
static uint64_t childMadeFrom(uint64_t parent, uint64_t file, int* several) {
    uint64_t match = 0;
    int matches = 0, n = 0;
    *several = 0;
    if (!file) return 0;
    for (uint64_t c = firstChildObjectOf(parent); c && n < PROMPT_MAX_CHILDREN;
         c = nextSiblingObjectOf(c), ++n)
        if (widgetFileOf(c) == file) { match = c; ++matches; }
    if (matches > 1) { *several = matches; return 0; }
    return match;
}

/* Title-cards as the prompt's only category of its own. */
static void keepTitleCardsOnly(uint64_t live) {
    unsigned own = 0;
    if (!readDword(live + WIDGET_OWN_CATEGORIES, &own) || own == TITLE_CARDS_BIT)
        return;
    GUARD_TRY {
        ((AssignCategoryFn)(global_moduleBase + ASSIGN_CATEGORY_RVA))(
            live, TITLE_CARDS_BIT);
    } GUARD_EXCEPT {}
    unsigned now = 0;
    readDword(live + WIDGET_OWN_CATEGORIES, &now);
    if (now != TITLE_CARDS_BIT)
        writeDword(live + WIDGET_OWN_CATEGORIES, TITLE_CARDS_BIT);
}

/* FNV-1a over a property's name, the hash the property setters compare. */
static uint64_t propertyHash(const char* name) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (; *name; ++name) {
        hash ^= (uint64_t)(int64_t)(signed char)*name;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

/* Sets a live widget's property through its own setter. */
static int setProperty(uint64_t widget, const char* name,
                       const BoxedValue* value) {
    const uint64_t method = widget ? methodOf(widget, WIDGET_SET_PROPERTY_SLOT)
                                   : 0;
    if (!method) return 0;
    int ok = 1;
    GUARD_TRY { ((SetPropertyFn)method)(widget, propertyHash(name), value); }
    GUARD_EXCEPT { ok = 0; }
    return ok;
}

static int setFloatProperty(uint64_t widget, const char* name, float number) {
    BoxedValue value = { 0, 0, BOXED_FLOAT };
    memcpy(&value.value, &number, sizeof(number));
    return setProperty(widget, name, &value);
}

static int setBoolProperty(uint64_t widget, const char* name, int on) {
    BoxedValue value = { on ? 1ull : 0ull, 0, BOXED_BOOL };
    return setProperty(widget, name, &value);
}

static int setTextProperty(uint64_t widget, const char* name, const char* text) {
    BoxedValue value = { (uint64_t)(uintptr_t)text, 0, BOXED_STRING };
    return setProperty(widget, name, &value);
}

/* ------------------------------------------------------------ the key */

/* The game's key code for each Windows key, 0 where it has none. */
static const uint8_t global_gameCodeOfKey[256] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    0xAB, 0x8F, 0, 0, 0, 0x99, 0, 0,
    0, 0, 0, 0xA9, 0, 0, 0, 0,
    0, 0, 0, 0x9C, 0, 0, 0, 0,
    0x20, 0xAD, 0xAE, 0xB0, 0xAF, 0xB1, 0xB3, 0xB2,
    0xB4, 0, 0, 0, 0, 0xAC, 0xAA, 0,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0, 0, 0, 0, 0, 0,
    0, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5A, 0, 0, 0, 0, 0,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8C, 0x8A, 0, 0x8B, 0x8E, 0x8D,
    0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4,
    0xA5, 0xA6, 0xA7, 0xA8, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0x90, 0x91, 0x93, 0x94, 0x96, 0x97, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0x3B, 0x3D, 0x2C, 0x2D, 0x2E, 0x2F,
    0x60, 0xB7, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0x5B, 0x5C, 0x5D, 0x27, 0xB8,
    0, 0, 0xB9, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};

/* A Windows key, by the name on it, for a key the game has no icon for. */
static const char* keyName(int key, char* out, size_t size) {
    static const struct { int key; const char* name; } named[] = {
        { 0x01, "Left Mouse" }, { 0x02, "Right Mouse" }, { 0x04, "Middle Mouse" },
        { 0x05, "Mouse 4" }, { 0x06, "Mouse 5" },
        { 0x08, "Backspace" }, { 0x09, "Tab" }, { 0x0D, "Enter" },
        { 0x10, "Shift" }, { 0x11, "Ctrl" }, { 0x12, "Alt" }, { 0x13, "Pause" },
        { 0x14, "Caps Lock" }, { 0x1B, "Esc" }, { 0x20, "Space" },
        { 0x21, "Page Up" }, { 0x22, "Page Down" }, { 0x23, "End" },
        { 0x24, "Home" }, { 0x25, "Left" }, { 0x26, "Up" }, { 0x27, "Right" },
        { 0x28, "Down" }, { 0x2C, "Print Screen" }, { 0x2D, "Insert" },
        { 0x2E, "Delete" }, { 0x5B, "Left Windows" }, { 0x5C, "Right Windows" },
        { 0x5D, "Menu" }, { 0x6A, "Num *" }, { 0x6B, "Num +" }, { 0x6D, "Num -" },
        { 0x6E, "Num ." }, { 0x6F, "Num /" }, { 0x90, "Num Lock" },
        { 0x91, "Scroll Lock" }, { 0xA0, "Left Shift" }, { 0xA1, "Right Shift" },
        { 0xA2, "Left Ctrl" }, { 0xA3, "Right Ctrl" }, { 0xA4, "Left Alt" },
        { 0xA5, "Right Alt" }, { 0xBA, "; :" }, { 0xBB, "= +" }, { 0xBC, ", <" },
        { 0xBD, "- _" }, { 0xBE, ". >" }, { 0xBF, "/ ?" }, { 0xC0, "` ~" },
        { 0xDB, "[ {" }, { 0xDC, "\\ |" }, { 0xDD, "] }" }, { 0xDE, "' \"" },
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); ++i)
        if (named[i].key == key) {
            strncpy_s(out, size, named[i].name, _TRUNCATE);
            return out;
        }
    if ((key >= 0x30 && key <= 0x39) || (key >= 0x41 && key <= 0x5A))
        sprintf_s(out, size, "%c", key);
    else if (key >= 0x60 && key <= 0x69)
        sprintf_s(out, size, "Num %d", key - 0x60);
    else if (key >= 0x70 && key <= 0x87)
        sprintf_s(out, size, "F%d", key - 0x6F);
    else
        sprintf_s(out, size, "Key %02X", key);
    return out;
}

/* What 020E0EE4 writes for the game's code for a key, large or not, into out;
   empty when it writes nothing or faults. */
static void keyIconPath(uint8_t code, int large, char* out, size_t room) {
    out[0] = 0;
    IconBuilder builder;
    memset(&builder, 0, sizeof(builder));
    builder.type = global_moduleBase + ICON_BUILDER_TYPE_RVA;
    builder.capacity = 0x100;
    int ok = 1;
    GUARD_TRY {
        ((KeyIconTextFn)(global_moduleBase + KEY_ICON_TEXT_RVA))(
            (uint64_t)code, &builder, large ? 1ull : 0ull);
    } GUARD_EXCEPT { ok = 0; }
    builder.text[sizeof(builder.text) - 1] = 0;
    if (ok && builder.length < 0x100) strcpy_s(out, room, builder.text);
}

/* What the game gives as the skip key's icon, and as its large picture,
   asked once for each key. A letter's icon is its lower case code's. */
static void findKeyIcon(void) {
    const int key = global_set.key;
    if (global_promptIconKey == key) return;
    global_promptIconKey = key;
    global_promptIcon[0] = global_promptIconLarge[0] = 0;
    global_promptIconIsPath = 0;
    uint8_t code = key > 0 && key < 256 ? global_gameCodeOfKey[key] : 0;
    if (!code) return;   /* the prompt shows the key's name */
    if (code >= 0x41 && code <= 0x5A) code = (uint8_t)(code + 0x20);
    keyIconPath(code, 0, global_promptIcon, sizeof(global_promptIcon));
    keyIconPath(code, 1, global_promptIconLarge, sizeof(global_promptIconLarge));
    global_promptIconIsPath = global_promptIcon[0]
                              && strstr(global_promptIcon, ".svg")
                              && !strchr(global_promptIcon, '<')
                              && !strchr(global_promptIcon, '|');
    /* Where there is no large picture 020E0EE4 falls back to the ordinary
       one's .svg, or names a .png that may not be there. */
    if (!strstr(global_promptIconLarge, "_large.svg"))
        global_promptIconLarge[0] = 0;
}

/* ------------------------------------------------- filling the prompt */

/* Sets one anchor pair, the one moving away from the middle first, so the
   minimum never passes the maximum. */
static void setAnchor(uint64_t prompt, const char* minName,
                      const char* maxName, float at) {
    if (at >= 0.5f) {
        setFloatProperty(prompt, maxName, at);
        setFloatProperty(prompt, minName, at);
    } else {
        setFloatProperty(prompt, minName, at);
        setFloatProperty(prompt, maxName, at);
    }
}

/* Puts the prompt in its corner: the anchor at the corner less the margins,
   the pivot at the prompt's own matching corner, no offset between them. */
static void placeCorner(uint64_t prompt) {
    const int corner = global_set.corner;
    const int right = corner == 2 || corner == 4;
    const int bottom = corner == 3 || corner == 4;
    setAnchor(prompt, "anchorMinN.x", "anchorMaxN.x",
              right ? 1.0f - PROMPT_MARGIN_X : PROMPT_MARGIN_X);
    setAnchor(prompt, "anchorMinN.y", "anchorMaxN.y",
              bottom ? 1.0f - PROMPT_MARGIN_Y : PROMPT_MARGIN_Y);
    setFloatProperty(prompt, "pivotN.x", right ? 1.0f : 0.0f);
    setFloatProperty(prompt, "pivotN.y", bottom ? 1.0f : 0.0f);
    setFloatProperty(prompt, "pivotAs.x", 0.0f);
    setFloatProperty(prompt, "pivotAs.y", 0.0f);
}

/* Finds the prompt's parts, in the file's order: the icon (the first Image),
   the text (the Text) and the probe (the second Image). */
static void findPromptParts(uint64_t prompt) {
    global_promptImage = global_promptLabel = global_promptProbe = 0;
    int n = 0;
    for (uint64_t part = firstChildObjectOf(prompt); part && n < 16;
         part = nextSiblingObjectOf(part), ++n) {
        const unsigned cls = tableRva(part);
        if (cls == IMAGE_CLASS_RVA) {
            if (!global_promptImage) global_promptImage = part;
            else if (!global_promptProbe) global_promptProbe = part;
        } else if (cls == TEXT_CLASS_RVA && !global_promptLabel)
            global_promptLabel = part;
    }
}

/* Shows the scene's text, with the key's name in front when the game gives no
   icon picture for the key; set only when it differs from the text last set.
   The text goes into one of four buffers in turn, so the one set last stays
   unchanged while it is shown. */
static void showPromptText(const char* label) {
    char text[256];
    if (global_promptIcon[0] && !global_promptIconIsPath)
        sprintf_s(text, "%s %s", global_promptIcon, label);
    else if (!global_promptIcon[0]) {
        char keyText[48];
        sprintf_s(text, "[%s] %s", keyName(global_set.key, keyText,
                                           sizeof(keyText)), label);
    } else
        strcpy_s(text, sizeof(text), label);
    if (!strcmp(text, global_promptTextNow)) return;
    char* keep = global_promptTexts[global_promptTextNext];
    global_promptTextNext = (global_promptTextNext + 1) % 4;
    strcpy_s(keep, sizeof(global_promptTexts[0]), text);
    if (setTextProperty(global_promptLabel, "value", keep))
        global_promptTextNow = keep;
}

/* The prompt's opacity, set when it changes. Its parts inherit it. */
static void setPromptAlpha(uint64_t prompt, float alpha) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    if (alpha == global_promptAlpha) return;
    if (setFloatProperty(prompt, "fcolor.a", alpha)) global_promptAlpha = alpha;
}

/* The opacity the prompt has now in its phase. */
static float promptAlphaNow(void) {
    const float opacity = (float)global_set.opacity / 100.0f;
    switch (global_promptPhase) {
    case PHASE_IN:
        return opacity * (float)global_promptPhaseMs / (float)PROMPT_FADE_IN_MS;
    case PHASE_HELD:
        return opacity;
    case PHASE_OUT:
        return opacity * (1.0f - (float)global_promptPhaseMs
                                 / (float)PROMPT_FADE_OUT_MS);
    default:
        return 0.0f;
    }
}

static int promptShowing(void) {
    return global_promptPhase == PHASE_IN || global_promptPhase == PHASE_HELD
        || global_promptPhase == PHASE_OUT;
}

/*
    The icon shows the skip key's picture when the game writes it as a path,
    and is hidden otherwise, the key's name then going in the text. Above 100%
    it is the large picture, where the key has one. The probe is given the
    ordinary picture, whose own size measurePromptIcon reads to size the
    icon's box.
*/
static void setPromptIcon(void) {
    findKeyIcon();
    global_promptIconMeasured = 0;
    global_promptIconChecks = 0;
    if (!global_promptImage) return;
    if (global_promptIcon[0] && global_promptIconIsPath) {
        const char* picture = global_set.size > 100 && global_promptIconLarge[0]
                                  ? global_promptIconLarge : global_promptIcon;
        setTextProperty(global_promptImage, "value", picture);
        setBoolProperty(global_promptImage, "visible", 1);
        if (global_promptProbe)
            setTextProperty(global_promptProbe, "value", global_promptIcon);
    } else
        setBoolProperty(global_promptImage, "visible", 0);
}

/*
    Sizes the icon's box to the key's picture: the probe's own width, as the
    engine laid it out, times the size; its height is the root's, from its
    anchors. The icon draws its picture StretchAspect in that box, so it keeps
    the picture's shape at any size. Until the probe has a size the box is the
    file's, 54 wide times the size.
*/
static void measurePromptIcon(void) {
    if (global_promptIconMeasured || !global_promptImage || !global_promptProbe
        || !global_promptIconIsPath
        || global_promptIconChecks >= PROMPT_MEASURE_CHECKS)
        return;
    float width = 0.0f, height = 0.0f;
    if (!readFloat(global_promptProbe + LAYOUT_RECT + 8, &width)
        || !readFloat(global_promptProbe + LAYOUT_RECT + 12, &height))
        return;
    if (width < 1.0f || height < 1.0f) {
        ++global_promptIconChecks;
        return;
    }
    const float f = (float)global_set.size / 100.0f;
    setFloatProperty(global_promptImage, "rectSizeDelta.width", width * f);
    global_promptIconMeasured = 1;
}

/* Fills a prompt newly found, whole: its corner, its icon, its opacity and
   whether it shows, as the phase has them. Its text follows from the offer,
   in the same check. */
static void applyPromptLook(uint64_t prompt) {
    placeCorner(prompt);
    setPromptIcon();
    global_promptTextNow = "";
    global_promptAlpha = -1.0f;
    setPromptAlpha(prompt, promptAlphaNow());
    setBoolProperty(prompt, "visible", promptShowing());
}

/* The prompt held, when it is still the one under the interface root made
   from the file in use; else 0. */
static uint64_t heldPrompt(void) {
    const uint64_t hud = hudRoot();
    const PromptFile* file = global_promptFileUsed;
    int several = 0;
    if (!global_promptLive || !hud || !file
        || childMadeFrom(hud, file->id, &several) != global_promptLive)
        return 0;
    return global_promptLive;
}

/* Hides the prompt held, if it is still there, and forgets it. */
static void dropPrompt(void) {
    const uint64_t live = heldPrompt();
    if (live) {
        setPromptAlpha(live, 0.0f);
        setBoolProperty(live, "visible", 0);
    }
    global_promptLive = 0;
    global_promptImage = global_promptLabel = global_promptProbe = 0;
    global_promptPhase = PHASE_HIDDEN;
    global_promptOffer = 0;
}

/*
    Shows the prompt while a skip is offered, with the scene's text. An offer
    with another target or text than the one shown fades it in; one that comes
    back within PROMPT_OFFER_GRACE_MS of being lost is the same offer, so a
    prompt that had faded out stays gone. No offer hides it at once.
*/
static void updatePrompt(uint64_t prompt) {
    uint64_t scene = 0, target = 0, checkpoint = 0, countedFrom = 0;
    int cinematic = 0, how = 0;
    const int offered = resolveSkip(&scene, &target, &checkpoint, &cinematic,
                                    &how, &countedFrom);
    const ULONGLONG now = GetTickCount64();
    if (!offered) {
        if (global_promptPhase != PHASE_HIDDEN) {
            setPromptAlpha(prompt, 0.0f);
            setBoolProperty(prompt, "visible", 0);
            global_promptLostOffer = global_promptOffer;
            global_promptLostAt = now;
            global_promptLostPhase = global_promptPhase;
            global_promptPhase = PHASE_HIDDEN;
            global_promptOffer = 0;
        }
        return;
    }
    const char* label = labelFor(scene, cinematic);
    const uint64_t offer = target ^ (idOfName(label) * 0x9E3779B97F4A7C15ull);
    showPromptText(label);
    if (offer == global_promptOffer) return;
    global_promptOffer = offer;
    if (offer == global_promptLostOffer
        && now - global_promptLostAt <= PROMPT_OFFER_GRACE_MS
        && global_promptLostPhase == PHASE_GONE) {
        global_promptPhase = PHASE_GONE;
        return;
    }
    global_promptPhase = PHASE_IN;
    global_promptPhaseMs = 0;
    global_promptLastTick = now;
    setPromptAlpha(prompt, 0.0f);
    setBoolProperty(prompt, "visible", 1);
}

static void makeSkipPrompt(uint64_t hud, ULONGLONG now) {
    const PromptFile* file = global_promptFileUsed;
    if (!file || !file->registered) {
        /* Not registered yet: the game's widget file load registers it. */
        if (global_promptFilesDone && !global_promptFilesSaid) {
            global_promptFilesSaid = 1;
            writeLog("PROMPT the widget file %s was not registered, so no "
                     "prompt is shown at this size; the skip key still skips",
                     file ? file->name : "for the prompt's size");
        }
        return;
    }
    /* The name the get-or-create takes: the instance name's id, then two
       zeros, as the game builds it for its own prompts. */
    uint64_t name[3] = { idOfName(PROMPT_NAME), 0, 0 };
    uint64_t w = 0;
    GUARD_TRY {
        w = ((GetOrCreateChildFn)(global_moduleBase + GET_OR_CREATE_CHILD_RVA))(
            hud, name, file->name, PROMPT_NAME, 0, 0, 0, 0);
    } GUARD_EXCEPT { w = 0; }
    global_promptLastMake = now;
    /* The gap before the next make doubles until a prompt is found. */
    global_promptMakeGap = global_promptMakeGap * 2 > PROMPT_MAKE_GAP_MAX_MS
                               ? PROMPT_MAKE_GAP_MAX_MS
                               : global_promptMakeGap * 2;
    if (!w && !global_promptMakeFailed) {
        global_promptMakeFailed = 1;
        writeLog("PROMPT the prompt could not be made from %s under the "
                 "interface root; it is tried again later", file->name);
    }
    global_promptFastUntil = now + PROMPT_FAST_MS;
}

/*
    Every PROMPT_CHECK_MS, and every tick for PROMPT_FAST_MS after a make or a
    find and while no prompt is held: finds the live prompt under the
    interface root, or makes one when there is none (at most once per the make
    gap); fills a prompt newly found whole, and fills again any part the game
    has replaced; sizes the icon once the probe has the picture's size; keeps
    title-cards its only category; and shows it while a skip is offered.
    Nothing is written to a widget this check did not find under the root.
*/
static void ensureSkipPrompt(void) {
    if (!promptOn()) return;
    const ULONGLONG now = GetTickCount64();
    const int fast = !global_promptLive || now < global_promptFastUntil;
    if (!fast && now < global_promptNextCheck) return;
    global_promptNextCheck = now + PROMPT_CHECK_MS;

    const uint64_t hud = hudRoot();
    const PromptFile* file = global_promptFileUsed;
    if (!hud || !file) return;

    int several = 0;
    const uint64_t live = childMadeFrom(hud, file->id, &several);
    if (several) {
        /* Not one to write to: which is shown cannot be told. */
        global_promptLive = 0;
        return;
    }
    if (!live) {
        global_promptLive = 0;
        if (now - global_promptLastMake >= global_promptMakeGap)
            makeSkipPrompt(hud, now);
        return;
    }

    if (live != global_promptLive) {
        global_promptLive = live;
        global_promptMakeGap = PROMPT_MAKE_GAP_MS;
        global_promptFastUntil = now + PROMPT_FAST_MS;
        findPromptParts(live);
        applyPromptLook(live);
    } else {
        /* Should the game replace a part, the part is filled again. */
        const uint64_t image = global_promptImage, label = global_promptLabel;
        const uint64_t probe = global_promptProbe;
        findPromptParts(live);
        if (image != global_promptImage || probe != global_promptProbe)
            setPromptIcon();
        if (label != global_promptLabel) global_promptTextNow = "";
    }
    keepTitleCardsOnly(live);
    updatePrompt(live);
    measurePromptIcon();
}

/*
    Every tick while the prompt fades in, is held or fades out: moves the fade
    on by the time since the last tick, counted only while the prompt is drawn,
    so neither the fade nor the seconds run under the pause menu. A prompt held
    Persistent stays until the skip is no longer offered. The prompt is
    written only while it is still the one under the interface root.
*/
static void fadeSkipPrompt(void) {
    if (!promptOn() || !global_promptLive || !promptShowing()) return;
    const uint64_t hud = hudRoot();
    const PromptFile* file = global_promptFileUsed;
    int several = 0;
    if (!hud || !file || childMadeFrom(hud, file->id, &several)
                         != global_promptLive)
        return;
    const uint64_t live = global_promptLive;
    const ULONGLONG now = GetTickCount64();
    ULONGLONG step = now - global_promptLastTick;
    global_promptLastTick = now;
    if (step > PROMPT_MAX_FADE_STEP_MS) step = PROMPT_MAX_FADE_STEP_MS;
    if (widgetDrawn(live) != 1) return;
    global_promptPhaseMs += step;

    if (global_promptPhase == PHASE_IN
        && global_promptPhaseMs >= (ULONGLONG)PROMPT_FADE_IN_MS) {
        global_promptPhase = PHASE_HELD;
        global_promptPhaseMs = 0;
    } else if (global_promptPhase == PHASE_HELD
               && global_set.seconds <= PROMPT_LONGEST_SECONDS
               && global_promptPhaseMs
                  >= (ULONGLONG)global_set.seconds * 1000ull) {
        global_promptPhase = PHASE_OUT;
        global_promptPhaseMs = 0;
    } else if (global_promptPhase == PHASE_OUT
               && global_promptPhaseMs >= (ULONGLONG)PROMPT_FADE_OUT_MS) {
        global_promptPhase = PHASE_GONE;
        setPromptAlpha(live, 0.0f);
        setBoolProperty(live, "visible", 0);
        return;
    }
    setPromptAlpha(live, promptAlphaNow());
}

/* ------------------------------------------------- a change of settings */

/*
    Takes up settings the menu has just written. The prompt is written only
    while it is still the one under the interface root.

      - The key, the enables or the duration turning the prompt off hide it.
      - A new size is another widget file: the prompt held is hidden and left
        where it is (the interface is rebuilt at the next load, and the size
        coming back finds it again), and a prompt is made from the new file.
      - A new corner, key or opacity is written to the prompt held.
*/
static void applySettings(const Settings* now) {
    const Settings was = global_set;
    global_set = *now;
    if (!global_gameKnown) return;
    const uint64_t live = heldPrompt();
    if (!live) global_promptLive = 0;

    if (!promptOn()) {
        dropPrompt();
        return;
    }
    if (now->size != was.size) {
        dropPrompt();
        global_promptFileUsed = promptFileFor(now->size);
        global_promptMakeGap = PROMPT_MAKE_GAP_MS;
        global_promptLastMake = 0;
        global_promptIconMeasured = 0;
        return;
    }
    if (!live) return;
    if (now->corner != was.corner) placeCorner(live);
    if (now->key != was.key) {
        setPromptIcon();
        global_promptTextNow = "";   /* the key's name may be in the text */
    }
    if (now->opacity != was.opacity) {
        global_promptAlpha = -1.0f;
        setPromptAlpha(live, promptAlphaNow());
    }
}

/* ------------------------------------------------------------ the tick */

static volatile LONG global_ticking = 0;

/* Every frame, from the game's script update, on the game's own thread. */
static void sceneSkipperTick(void* registers, void* user) {
    (void)registers; (void)user;
    if (InterlockedCompareExchange(&global_ticking, 1, 0) != 0) return;
    Settings fresh;
    if (settingsChanged(&fresh)) applySettings(&fresh);
    if (global_gameKnown && global_set.enabled) {
        global_currentTask = currentTaskNode();
        updatePauseOffer();
        updateSpeedrunMark();
        keepPauseWords();
        skipWhenAsked();
        ensureSkipPrompt();
        fadeSkipPrompt();
    } else {
        InterlockedExchange(&global_pauseOffer, 0);
        InterlockedExchange(&global_pauseSkipAsked, 0);
        if (global_gameKnown) keepPauseWords();
    }
    InterlockedExchange(&global_ticking, 0);
}

/* ------------------------------------------------------------------ plugin */

/*
    What the Scene Skipper is doing, for the Dev Toolkit's [tasks]
    watchSceneSkipper; it logs nothing of this itself. The asker sets size to
    its own sizeof(SceneSkipperView); a size other than this one's is answered
    with 0, so a toolkit built against another layout reads nothing wrong.
*/
struct SceneSkipperView {
    uint32_t size;          /* the asker's sizeof(SceneSkipperView) */
    uint32_t offered;       /* 1 while a skip is offered */
    uint64_t scene;         /* the scene a skip is made during, as it counts it */
    uint64_t countedFrom;   /* the scene the cinematic names, when it counts as another */
    uint64_t target;        /* the task a skip goes to */
    uint32_t how;           /* 1 its table names it, 2 the game offers it */
    uint32_t cinematic;     /* 1 while a cinematic plays */
    float    moveTo;        /* the frame a press moves the cinematic to; 0 for none */
    uint32_t phase;         /* the prompt: 0 hidden, 1 in, 2 held, 3 out, 4 gone */
    uint32_t skips;         /* the skips made since the game started */
    uint32_t lastHow;       /* the last skip: 1 or 2 to a task, 3 a move */
    uint64_t lastScene;     /* the scene it was made during */
    uint64_t lastTarget;    /* the task it went to; 0 for a move */
    float    lastFrame;     /* the frame a move went to, and the frame it was at */
    float    lastFrom;
    char     label[64];     /* the prompt's text now */
    uint32_t pauseOffered;  /* 1 while the pause menu offers its row for this skip */
    uint32_t lastFromPause; /* 1 when the last skip was chosen in the pause menu */
    float    lastTimeAdded; /* the seconds it added to the speedrun timer */
};

extern "C" TLOU_PLUGIN_EXPORT int SceneSkipperDescribe(SceneSkipperView* view) {
    if (!view || view->size != (uint32_t)sizeof(SceneSkipperView)) return 0;
    memset(view, 0, sizeof(*view));
    view->size = (uint32_t)sizeof(SceneSkipperView);
    if (global_gameKnown && global_set.enabled) {
        uint64_t checkpoint = 0;
        int cinematic = 0, how = 0;
        view->offered = (uint32_t)resolveSkip(&view->scene, &view->target,
                                              &checkpoint, &cinematic, &how,
                                              &view->countedFrom);
        view->how = (uint32_t)how;
        view->cinematic = (uint32_t)cinematic;
        if (view->offered) view->moveTo = moveFrameFor(view->scene, cinematic);
        if (view->offered && promptOn())
            strncpy_s(view->label, sizeof(view->label),
                      labelFor(view->scene, cinematic), _TRUNCATE);
    }
    view->phase = promptOn() && global_promptLive ? (uint32_t)global_promptPhase
                                                  : 0;
    view->skips = global_skipsMade;
    view->lastHow = global_lastHow;
    view->lastScene = global_lastScene;
    view->lastTarget = global_lastTarget;
    view->lastFrame = global_lastFrame;
    view->lastFrom = global_lastFrom;
    view->pauseOffered = global_pauseHooked && global_pauseOffer ? 1u : 0u;
    view->lastFromPause = global_lastFromPause;
    view->lastTimeAdded = global_lastTimeAdded;
    return 1;
}

extern "C" TLOU_PLUGIN_EXPORT const char* PluginName(void) { return "Scene Skipper"; }
extern "C" TLOU_PLUGIN_EXPORT const char* PluginVersion(void) { return "v1.0.0"; }
extern "C" TLOU_PLUGIN_EXPORT const char* PluginAuthor(void)  { return "mwith77"; }

extern "C" TLOU_PLUGIN_EXPORT int SupportsGame(LoaderHandle) { return 1; }

static volatile LONG global_initCalled = 0;

static DWORD WINAPI standalone(LPVOID) {
    Sleep(2000);
    if (!global_initCalled)
        writeLog("no loader present; nothing can be hooked");
    return 0;
}

static void pathsFrom(const char* modFolder, const char* logsFolder) {
    formatInto(global_iniPath, sizeof(global_iniPath), "%s\\%s.ini", modFolder,
               modBaseName);
    formatInto(global_logPath, sizeof(global_logPath), "%s\\%s.log",
               logsFolder, modBaseName);
}

extern "C" TLOU_PLUGIN_EXPORT int Init(LoaderHandle loader) {
    InterlockedExchange(&global_initCalled, 1);
    if (!loaderBind(loader)) return 0;
    /* Before anything else: a bounded call handed too much is reported
       through the loader instead of ending the process. */
    loaderWatchBadArguments(PluginName());

    const char* folder = loaderModFolder();
    const char* logs = loaderLogsFolder();
    if (folder && *folder && logs && *logs) {
        pathsFrom(folder, logs);
        archiveLog();
    }

    /* The settings' write time must be taken before they are read. */
    iniWrittenAt(&global_iniReadAt);
    readSettings(&global_set);
    nameTheTables();

    global_moduleBase = loaderGameModuleBase();
    global_moduleSize = loaderGameModuleSize();
    global_textBase = loaderGameTextBase();
    global_textSize = loaderGameTextSize();
    if (!global_moduleBase || !global_textSize) {
        writeLog("the loader did not give the game's place in memory, so the "
                 "Scene Skipper does nothing");
        return 0;
    }
    global_gameKnown = loaderGameTimeDateStamp() == GAME_BUILD_STAMP;
    if (!global_gameKnown) {
        writeLog("the game is not tlou-i.exe 1.1.5.0 (its time stamp is %08X), "
                 "the build the Scene Skipper's addresses are for, so it does "
                 "nothing", loaderGameTimeDateStamp());
        return 1;
    }

    const uint64_t found = loaderFindFunction("GameTaskSubnode::SkipTo",
                                              skipToSignature);
    GUARD_TRY { global_skipToFunction = found ? entryOfFunction(found) : 0; }
    GUARD_EXCEPT { global_skipToFunction = 0; }
    if (!global_skipToFunction)
        writeLog("GameTaskSubnode::SkipTo was not found, so the skip key makes "
                 "no skips to a task");

    installPromptFiles();

    if (!loaderHookFunction("ScriptManager::ListenerUpdate",
                            listenerUpdateSignature, sceneSkipperTick, nullptr)) {
        writeLog("the loader refused the hook on ScriptManager::ListenerUpdate, "
                 "so the Scene Skipper does nothing");
        return 1;
    }
    installPauseMenuSkip();
    return 1;
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);

    /* Paths for a start without the loader; Init replaces them. */
    char folder[MAX_PATH], logs[MAX_PATH];
    const DWORD length = GetModuleFileNameA(self, folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return TRUE;
    char* slash = strrchr(folder, '\\');
    if (slash) *slash = 0;
    if (formatInto(logs, sizeof(logs), "%s\\Logs", folder)) {
        CreateDirectoryA(logs, nullptr);
        pathsFrom(folder, logs);
    }

    CloseHandle(CreateThread(nullptr, 0, standalone, nullptr, 0, nullptr));
    return TRUE;
}
