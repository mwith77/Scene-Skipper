#include <windows.h>
#include <intrin.h>
#include <cstdarg>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#undef near
#undef far

#define TLOU_PLUGIN
#include "tlou_plugin_sdk.h"

/* GetAsyncKeyState, GetForegroundWindow and GetWindowThreadProcessId, for the
   skip key in The Last of Us Part II. */
#pragma comment(lib, "user32.lib")

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

/*
    The log is one file in the mod's own folder, newest line first: each line
    is written above the lines already there, the earlier runs' included. It
    is at most LOG_MOST_BYTES; the oldest lines, at its end, are left out to
    keep it there. Each line is written through <log>.new, renamed over the
    log. Every line carries the machine's local date and time.
*/
#define LOG_MOST_BYTES (1024u * 1024u)
#define LOG_LINE_MOST  4096

static SRWLOCK global_logLock = SRWLOCK_INIT;

static void writeLog(const char* fmt, ...) {
    if (!global_logPath[0]) return;
    char line[LOG_LINE_MOST];
    SYSTEMTIME now;
    GetLocalTime(&now);
    int used = sprintf_s(line, "%04u-%02u-%02u %02u:%02u:%02u.%03u  ",
                         now.wYear, now.wMonth, now.wDay, now.wHour,
                         now.wMinute, now.wSecond, now.wMilliseconds);
    if (used < 0) return;
    va_list ap;
    va_start(ap, fmt);
    const int text = _vsnprintf_s(line + used, sizeof(line) - used - 1,
                                  _TRUNCATE, fmt, ap);
    va_end(ap);
    used = text < 0 ? (int)strlen(line) : used + text;
    line[used++] = '\n';

    AcquireSRWLockExclusive(&global_logLock);
    char* out = (char*)malloc(LOG_MOST_BYTES);
    if (out) {
        memcpy(out, line, (size_t)used);
        size_t total = (size_t)used;
        const HANDLE old = CreateFileA(global_logPath, GENERIC_READ,
                                       FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        if (old != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            const DWORD room = (DWORD)(LOG_MOST_BYTES - total);
            if (ReadFile(old, out + total, room, &got, nullptr)) {
                LARGE_INTEGER size;
                if (GetFileSizeEx(old, &size) && size.QuadPart > (LONGLONG)got)
                    while (got > 0 && out[total + got - 1] != '\n') --got;
                total += got;
            }
            CloseHandle(old);
        }
        char fresh[MAX_PATH];
        if (sprintf_s(fresh, "%s.new", global_logPath) > 0) {
            const HANDLE file = CreateFileA(fresh, GENERIC_WRITE, 0, nullptr,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                            nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                const BOOL ok = WriteFile(file, out, (DWORD)total, &wrote, nullptr)
                             && wrote == (DWORD)total;
                CloseHandle(file);
                if (ok) MoveFileExA(fresh, global_logPath, MOVEFILE_REPLACE_EXISTING);
                else DeleteFileA(fresh);
            }
        }
        free(out);
    }
    ReleaseSRWLockExclusive(&global_logLock);
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

/* The date and the time a time stamp names, written into out as
   YYYY-MM-DD HH:MM:SS UTC. The stamp counts seconds from the start of
   1970-01-01, UTC. */
static void writeStampDate(char* out, size_t room, uint32_t stamp) {
    const __time64_t seconds = stamp;
    struct tm when;
    if (_gmtime64_s(&when, &seconds) != 0) {
        formatInto(out, room, "an unknown date");
        return;
    }
    formatInto(out, room, "%04d-%02d-%02d %02d:%02d:%02d UTC",
               when.tm_year + 1900, when.tm_mon + 1, when.tm_mday,
               when.tm_hour, when.tm_min, when.tm_sec);
}

/* --------------------------------------------------- the default settings */

/*
    The default settings are the RCDATA resource DEFAULT_SETTINGS: the
    plugin's ini as it stands in the project. Read as an ini, a line [name]
    opens a section, a line key = value below it is a setting of that
    section, and a line opening with ; is a comment. The blanks around a name,
    a key or a value are not part of it. Lines end in CR, LF or CR LF.
*/

/* The image this code is linked into; its address is its module handle. */
extern "C" IMAGE_DOS_HEADER __ImageBase;

/* The three bytes of a UTF-8 byte-order mark. */
static const char utf8ByteOrderMark[] = "\xEF\xBB\xBF";

/* The default settingIsPresent gives GetPrivateProfileStringA. No value in a
   settings file is this text. */
static const char missingSettingMark[] = "\x01missing\x01";

/*
    The RCDATA resource DEFAULT_SETTINGS of module, less a UTF-8 byte-order
    mark at its start: *length bytes, not ended by a zero. nullptr, with 0 in
    *length, where module holds no such resource or it holds nothing more.
*/
static const char* defaultSettingsText(HMODULE module, DWORD* length) {
    *length = 0;
    const HRSRC found = FindResourceA(module, "DEFAULT_SETTINGS",
                                      (LPCSTR)RT_RCDATA);
    const HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
    const char* text = loaded ? (const char*)LockResource(loaded) : nullptr;
    if (!text) return nullptr;
    DWORD size = SizeofResource(module, found);
    if (size >= 3 && memcmp(text, utf8ByteOrderMark, 3) == 0) {
        text += 3;
        size -= 3;
    }
    if (!size) return nullptr;
    *length = size;
    return text;
}

/*
    1 where the settings file at path holds key in section, whatever its
    value, an empty one included; 0 where Windows reports the key absent
    (ERROR_FILE_NOT_FOUND, which also stands for no such section and no such
    file); -1 where the file could not be read, for example while another
    program holds it open without read sharing (ERROR_SHARING_VIOLATION),
    with the error in GetLastError.
*/
static int settingIsPresent(const char* path, const char* section,
                            const char* key) {
    char held[sizeof(missingSettingMark) + 1];
    SetLastError(0);
    GetPrivateProfileStringA(section, key, missingSettingMark, held,
                             (DWORD)sizeof(held), path);
    const DWORD error = GetLastError();
    if (strcmp(held, missingSettingMark) != 0) return 1;
    if (error == ERROR_FILE_NOT_FOUND) return 0;
    SetLastError(error);
    return -1;
}

/* One setting of the default settings: its section, key and value, each
   without the blanks at its ends. */
struct DefaultSetting {
    char section[256];
    char key[256];
    char value[512];
};

/* The text from first up to last, without the blanks at its ends, copied
   into out with a zero after it. Answers 0, with out empty, where it does not
   fit in room. */
static int copyTrimmed(char* out, size_t room, const char* first,
                       const char* last) {
    while (first < last && (*first == ' ' || *first == '\t')) ++first;
    while (last > first && (last[-1] == ' ' || last[-1] == '\t')) --last;
    out[0] = 0;
    if ((size_t)(last - first) >= room) return 0;
    memcpy(out, first, (size_t)(last - first));
    out[last - first] = 0;
    return 1;
}

/*
    The next setting of the default settings text from *at up to end, read
    into setting, with *at moved past its line. Answers 0 once no setting is
    left. setting->section is empty before the first call and then holds the
    section last opened. A line with no =, a setting before the first section
    or below a [ line with no ], and a line too long for setting are passed
    over.
*/
static int nextDefaultSetting(const char** at, const char* end,
                              DefaultSetting* setting) {
    while (*at < end) {
        const char* first = *at;
        const char* last = first;
        while (last < end && *last != '\r' && *last != '\n') ++last;
        *at = last;
        while (*at < end && (**at == '\r' || **at == '\n')) ++*at;

        while (first < last && (*first == ' ' || *first == '\t')) ++first;
        if (first == last || *first == ';') continue;
        if (*first == '[') {
            const char* close = (const char*)memchr(first, ']',
                                                    (size_t)(last - first));
            if (!close
                || !copyTrimmed(setting->section, sizeof(setting->section),
                                first + 1, close))
                setting->section[0] = 0;
            continue;
        }
        const char* equals = (const char*)memchr(first, '=',
                                                 (size_t)(last - first));
        if (!setting->section[0] || !equals
            || !copyTrimmed(setting->key, sizeof(setting->key), first, equals)
            || !setting->key[0]
            || !copyTrimmed(setting->value, sizeof(setting->value), equals + 1,
                            last))
            continue;
        return 1;
    }
    return 0;
}

/*
    Adds to the settings file at path each setting of the default settings
    text (length bytes) that Windows reports absent from it, in its section
    with its default value, through WritePrivateProfileStringA. A setting the
    file holds, an empty value included, and every other line of the file
    are left as they are. Stops at the first setting that cannot be looked up
    or added, with one line in the log.
*/
static void addMissingSettings(const char* path, const char* text,
                               DWORD length) {
    const char* at = text;
    DefaultSetting setting = {};
    while (nextDefaultSetting(&at, text + length, &setting)) {
        const int present = settingIsPresent(path, setting.section,
                                             setting.key);
        if (present > 0) continue;
        if (present < 0) {
            const DWORD error = GetLastError();
            writeLog("SETTINGS %s could not be read (Windows error %lu), so "
                     "[%s] %s and the settings after it were not looked up, "
                     "and those it lacks take their default values", path,
                     error, setting.section, setting.key);
            return;
        }
        if (!WritePrivateProfileStringA(setting.section, setting.key,
                                        setting.value, path)) {
            const DWORD error = GetLastError();
            writeLog("SETTINGS [%s] %s could not be added to %s (Windows error "
                     "%lu), so it and the settings after it that the file "
                     "lacks take their default values", setting.section,
                     setting.key, path, error);
            return;
        }
    }
}

/*
    Gives the settings file at path the default settings built into module:
    - no file, or an empty one (0 bytes): the text whole, comments included,
      written to <path>.new, which is flushed, closed and moved to path. The
      move never replaces a file. An empty file is first deleted, through a
      handle no other may share, and only while that handle shows it empty;
      an empty file that cannot be deleted is left as it is. Where a file is
      at path at the time of the move, the .new file is deleted and the
      settings that file lacks are added instead.
    - a file that starts with a UTF-8 byte-order mark: nothing.
    - any other file: each setting it lacks, added by addMissingSettings.
    Each failure writes one line to the log; a .new file this wrote is
    deleted after one.
*/
static void writeDefaultSettings(HMODULE module, const char* path) {
    if (!path || !path[0]) return;
    DWORD length = 0;
    const char* const text = defaultSettingsText(module, &length);
    if (!text) {
        writeLog("SETTINGS the default settings are not built into this DLL, "
                 "so none were written to %s", path);
        return;
    }

    /* The file's size and first three bytes, read under a shared lock on the
       whole file, which waits while WritePrivateProfileStringA writes it. */
    int empty = 0;
    const HANDLE file = CreateFileA(path, GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE
                                        | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            writeLog("SETTINGS %s could not be read (Windows error %lu), so "
                     "no settings were added to it, and those it lacks take "
                     "their default values", path, error);
            return;
        }
    } else {
        OVERLAPPED whole = {};
        LARGE_INTEGER size = {};
        char first[3] = {};
        DWORD got = 0;
        const BOOL locked = LockFileEx(file, 0, 0, MAXDWORD, MAXDWORD, &whole);
        const BOOL looked = locked && GetFileSizeEx(file, &size)
                            && ReadFile(file, first, (DWORD)sizeof(first),
                                        &got, nullptr);
        const DWORD error = looked ? ERROR_SUCCESS : GetLastError();
        if (locked) UnlockFileEx(file, 0, MAXDWORD, MAXDWORD, &whole);
        CloseHandle(file);
        if (!looked) {
            writeLog("SETTINGS %s could not be read (Windows error %lu), so "
                     "no settings were added to it, and those it lacks take "
                     "their default values", path, error);
            return;
        }
        if (size.QuadPart > 0) {
            if (got == sizeof(first)
                && memcmp(first, utf8ByteOrderMark, sizeof(first)) == 0)
                writeLog("SETTINGS %s starts with a UTF-8 byte-order mark, "
                         "which Windows reads as part of its first line, so no "
                         "settings were added to it", path);
            else
                addMissingSettings(path, text, length);
            return;
        }
        empty = 1;
    }

    char newPath[MAX_PATH];
    if (_snprintf_s(newPath, sizeof(newPath), _TRUNCATE, "%s.new", path) < 0) {
        writeLog("SETTINGS %s is too long a path to write the default "
                 "settings beside it", path);
        return;
    }
    const HANDLE out = CreateFileA(newPath, GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        writeLog("SETTINGS %s could not be made (Windows error %lu), so the "
                 "default settings were not written to %s", newPath, error,
                 path);
        return;
    }
    DWORD written = 0;
    DWORD error = ERROR_SUCCESS;
    if (!WriteFile(out, text, length, &written, nullptr)) error = GetLastError();
    else if (written != length) error = ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(out)) error = GetLastError();
    if (!CloseHandle(out) && error == ERROR_SUCCESS) error = GetLastError();

    if (error == ERROR_SUCCESS) {
        /* Why an empty file at path stays: ERROR_SUCCESS where it was
           deleted, is gone, or holds something by now. */
        DWORD kept = ERROR_SUCCESS;
        if (empty) {
            const HANDLE old = CreateFileA(path, DELETE | FILE_READ_ATTRIBUTES,
                                           0, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (old == INVALID_HANDLE_VALUE) {
                if (GetLastError() != ERROR_FILE_NOT_FOUND) kept = GetLastError();
            } else {
                LARGE_INTEGER size = {};
                FILE_DISPOSITION_INFO gone = { TRUE };
                if (GetFileSizeEx(old, &size) && size.QuadPart == 0
                    && !SetFileInformationByHandle(old, FileDispositionInfo,
                                                   &gone, (DWORD)sizeof(gone)))
                    kept = GetLastError();
                CloseHandle(old);
            }
        }
        if (kept != ERROR_SUCCESS) {
            DeleteFileA(newPath);
            writeLog("SETTINGS %s is empty and could not be replaced (Windows "
                     "error %lu), so the default settings were not written to "
                     "it, and the settings take their default values", path,
                     kept);
            return;
        }
        if (MoveFileExA(newPath, path, 0)) return;
        error = GetLastError();
    }
    DeleteFileA(newPath);
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
        addMissingSettings(path, text, length);
        return;
    }
    writeLog("SETTINGS the default settings could not be written to %s "
             "(Windows error %lu), so the settings take their default values",
             path, error);
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

/* Whether the length bytes from address are committed memory that can be
   read and written, and no guard page, as Windows reports it. Reads none of
   them. */
static int writableMemory(uint64_t address, uint64_t length) {
    if (!length || address < SMALLEST_POINTER || address > LARGEST_POINTER
        || length - 1 > LARGEST_POINTER - address)
        return 0;
    const uint64_t end = address + length;
    for (uint64_t at = address; at < end; ) {
        MEMORY_BASIC_INFORMATION region;
        if (VirtualQuery((const void*)(uintptr_t)at, &region, sizeof(region))
                != sizeof(region)
            || region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD))
            return 0;
        const DWORD access =
            region.Protect & ~(DWORD)(PAGE_NOCACHE | PAGE_WRITECOMBINE);
        if (access != PAGE_READWRITE && access != PAGE_WRITECOPY
            && access != PAGE_EXECUTE_READWRITE
            && access != PAGE_EXECUTE_WRITECOPY)
            return 0;
        at = (uint64_t)(uintptr_t)region.BaseAddress + region.RegionSize;
    }
    return 1;
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
    tlou-ii.exe and tlou-ii-l.exe 1.6.0 of The Last of Us Part II, by theirs.
*/
#define GAME_BUILD_STAMP           0x685BEBE1u
#define PART_TWO_STAMP             0x687DF976u   /* tlou-ii.exe 1.6.0 */
#define PART_TWO_L_STAMP           0x687DFAA4u   /* tlou-ii-l.exe 1.6.0 */

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

static int global_gameKnown = 0;   /* 1 on tlou-i.exe 1.1.5.0, the build the
                                       fixed addresses are for */
static int global_partTwo = 0;     /* 1 in Part II with every part of its skip
                                       found */

/* The process a handle names, its process id at processId in it, or 0 when
   it names nothing now. */
static uint64_t objectOfHandle(uint64_t handle, unsigned processId) {
    uint64_t record = 0, object = 0;
    unsigned id = 0, idNow = 0;
    if (!readQword(handle + HANDLE_RECORD, &record) || !record) return 0;
    if (!readDword(handle + HANDLE_ID, &id) || !id) return 0;
    if (!readQword(record + RECORD_OBJECT, &object) || !object) return 0;
    if (!readDword(object + processId, &idNow) || idNow != id) return 0;
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

/* ------------------------------------------------------- scene table */

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

/* A task by name for the log: the scene table's name for its id, else the
   words for the task the game's own Skip Cinematic goes to. */
static const char* taskNameFor(uint64_t task) {
    for (int i = 0; i < SCENE_COUNT; ++i) {
        if (global_sceneTable[i].targetId == task)
            return global_sceneTable[i].target;
        if (global_sceneTable[i].sceneId == task)
            return global_sceneTable[i].scene;
    }
    return "the task the game's own Skip Cinematic goes to";
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
    return objectOfHandle(manager + MANAGER_CINEMATIC, PROCESS_ID);
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
                                           + MENU_ROOT_HANDLE_RVA, PROCESS_ID);
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
            writeLog("SKIP the game refused to play the skip's target, %s, "
                     "from its checkpoint", taskNameFor(global_skipTarget));
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
        writeLog("SKIP the running task graph does not hold the skip's "
                 "target, %s", taskNameFor(target));
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

/* ------------------------------------------------- pause menu skip */

/*
    The pause menu offers its own Skip Cinematic row during the scenes the
    Scene Skipper skips where no cinematic plays (the study and the car ride),
    and Yes in the box it opens makes the Scene Skipper's skip, the one the
    key makes. During a cinematic the row and its skip stay the game's own.
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

/* Whether the count bytes at at are the loader's jump over that many
   displaced bytes: E9 and its 32-bit displacement, then 90 for each byte
   after the fifth. */
static int loaderJumpOver(const uint8_t* at, size_t count) {
    if (count < 5 || at[0] != 0xE9) return 0;
    for (size_t i = 5; i < count; ++i)
        if (at[i] != 0x90) return 0;
    return 1;
}

/* One of the hooks, placed where the game's bytes are those expected, or
   where the loader's jump for another plugin's hook at the same place
   stands over them; what names the place in words, and without says what
   is lost when it cannot be placed. */
static int hookPauseMenu(const char* name, const char* what, uint64_t rva,
                         const uint8_t* bytes, size_t count,
                         LoaderHookCallback callback, const char* without) {
    int same = 0;
    GUARD_TRY {
        const uint8_t* at = (const uint8_t*)(uintptr_t)(global_moduleBase + rva);
        same = memcmp(at, bytes, count) == 0 || loaderJumpOver(at, count);
    } GUARD_EXCEPT { same = 0; }
    if (!same) {
        writeLog("PAUSE the game's code %s is not what the Scene Skipper "
                 "expects, so %s", what, without);
        return 0;
    }
    if (!loaderHookAddress(name, global_moduleBase + rva, callback, nullptr)) {
        writeLog("PAUSE the loader refused the hook %s, so %s", what, without);
        return 0;
    }
    return 1;
}

/* The hooks, Yes's first and the row's entry last, so a row is only ever
   offered with everything in place to make its skip; then the words. */
static void installPauseMenuSkip(void) {
    static const char* const noSkip = "the pause menu offers no skip of its own";
    if (!global_skipToFunction) return;
    if (!hookPauseMenu("sceneskipper_pause_confirmed",
                       "where Yes in the pause menu's skip box is handled",
                       PAUSE_CONFIRMED_RVA, global_pauseConfirmedBytes,
                       sizeof(global_pauseConfirmedBytes), pauseConfirmed, noSkip))
        return;
    if (!hookPauseMenu("sceneskipper_pause_offered_exit",
                       "where the game's Skip Cinematic test answers",
                       PAUSE_OFFERED_EXIT_RVA, global_pauseOfferedExitBytes,
                       sizeof(global_pauseOfferedExitBytes), pauseOfferedExit,
                       noSkip))
        return;
    global_pauseHooked = hookPauseMenu("sceneskipper_pause_offered_entry",
                                       "where the game's Skip Cinematic test "
                                       "begins",
                                       PAUSE_OFFERED_ENTRY_RVA,
                                       global_pauseOfferedEntryBytes,
                                       sizeof(global_pauseOfferedEntryBytes),
                                       pauseOfferedEntry, noSkip);
    if (!global_pauseHooked) return;
    global_pauseTitle.id = idOfName(global_pauseTitle.name);
    global_pauseQuestion.id = idOfName(global_pauseQuestion.name);
    global_textHooked = hookPauseMenu("sceneskipper_text_answered",
                                      "where the game's text lookup answers",
                                      TEXT_ANSWERED_RVA,
                                      global_textAnsweredBytes,
                                      sizeof(global_textAnsweredBytes),
                                      textAnswered,
                                      "the pause menu's skip keeps the game's "
                                      "words");
}

/* ------------------------------------------------------------ the prompt */

/* The game's parts the prompt uses in Part I (tlou-i.exe 1.1.5.0), each
   explained in PromptParts. Part II's are found in its code by
   findPartTwoPrompt. */
#define GET_OR_CREATE_CHILD_RVA  0x0254A1D8ull
#define ASSIGN_CATEGORY_RVA      0x025450FCull
#define KEY_ICON_TEXT_RVA        0x020E0EE4ull
#define ICON_BUILDER_TYPE_RVA    0x02C79208ull
#define FIRST_CHILD_OBJECT_RVA   0x02549FC0ull
#define NEXT_SIBLING_OBJECT_RVA  0x0254A190ull
#define WIDGET_FILE_ID           0x1A8
#define WIDGET_OWN_CATEGORIES    0x2E8
#define WIDGET_IS_VISIBLE_SLOT   0x320
#define WIDGET_SET_PROPERTY_SLOT 0x380
#define LAYOUT_RECT              0x248
#define TEXT_CLASS_RVA           0x02EB83D0ull
#define IMAGE_CLASS_RVA          0x02EB7F00ull
#define TITLE_CARDS_BIT          (1u << 21)

/* The game's parts the prompt uses: functions and class tables by their
   address in the game, a widget's fields by their place in the widget, and
   its methods by their place in its class table. */
struct PromptParts {
    uint64_t getOrCreate;     /* (parent, &{the instance name's id, 0, 0}, the
                                 widget file's name, the instance name, 0, 0,
                                 0, 0) -> the child of parent made from that
                                 file under that name, made when there is none */
    uint64_t assignCategory;  /* (widget, categories): adds them to its own */
    uint64_t keyIconText;     /* (the game's code for a key, &IconBuilder,
                                 large): writes the key's icon as text */
    uint64_t builderClass;    /* the class of the builder it writes into */
    uint64_t firstChild;      /* (container) -> its first child, of any kind */
    uint64_t nextSibling;     /* (child) -> the next */
    unsigned widgetFileId;    /* a qword: the id of the widget file a widget
                                 was made from */
    unsigned ownCategories;   /* a dword: a widget's own categories */
    unsigned isVisibleSlot;   /* IsVisible: (widget) -> the engine draws it */
    unsigned setPropertySlot; /* the property setter: (widget, the property's
                                 id, &BoxedValue) */
    unsigned layoutRect;      /* four floats: the rectangle laid out, left,
                                 top, width and height */
    uint64_t textClass;       /* a Text's class table */
    uint64_t imageClass;      /* an Image's class table */
    unsigned titleCards;      /* the category title-cards, a bit */
    uint64_t hudHandle;       /* the interface root's handle */
    unsigned hudProcessId;    /* in the interface root: its process id, which
                                 the handle's id must equal */
};

static PromptParts global_promptParts;
static int         global_promptPartsFound = 0;   /* 1 once it is filled */

/* Part I's parts, the fixed ones. */
static void fillPartOnePrompt(void) {
    PromptParts* p = &global_promptParts;
    p->getOrCreate = global_moduleBase + GET_OR_CREATE_CHILD_RVA;
    p->assignCategory = global_moduleBase + ASSIGN_CATEGORY_RVA;
    p->keyIconText = global_moduleBase + KEY_ICON_TEXT_RVA;
    p->builderClass = global_moduleBase + ICON_BUILDER_TYPE_RVA;
    p->firstChild = global_moduleBase + FIRST_CHILD_OBJECT_RVA;
    p->nextSibling = global_moduleBase + NEXT_SIBLING_OBJECT_RVA;
    p->widgetFileId = WIDGET_FILE_ID;
    p->ownCategories = WIDGET_OWN_CATEGORIES;
    p->isVisibleSlot = WIDGET_IS_VISIBLE_SLOT;
    p->setPropertySlot = WIDGET_SET_PROPERTY_SLOT;
    p->layoutRect = LAYOUT_RECT;
    p->textClass = global_moduleBase + TEXT_CLASS_RVA;
    p->imageClass = global_moduleBase + IMAGE_CLASS_RVA;
    p->titleCards = TITLE_CARDS_BIT;
    p->hudHandle = global_moduleBase + HUD_HANDLE_RVA;
    p->hudProcessId = PROCESS_ID;
    global_promptPartsFound = 1;
}

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
/* The text builder the game makes on its stack for the key icon writer: its
   class, its capacity (256), the length written, and the text. */
struct IconBuilder {
    uint64_t type;
    uint64_t capacity;
    uint64_t length;
    char     text[0x108];
};
/* The key icon writer (the game's code for the key, the builder, large):
   large asks for html/pc/<name>_large.svg first, falling back to
   <name>.svg. */
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
static char      global_promptIcon[256];       /* what the key icon writer gives for the skip key */
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
    game starts. Without them no prompt is made; the key still skips. 1 when
    the registrar is hooked; 0, with one line in the log, when it is not.
*/
static int installPromptFiles(void) {
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
        return 0;
    }
    global_registerWidgetFile = loaderHookTrampoline(REGISTRAR_HOOK_NAME);
    return 1;
}

/* ------------------------------------------------- the prompt's widgets */

static uint64_t firstChildObjectOf(uint64_t container) {
    if (container < SMALLEST_POINTER || !global_promptParts.firstChild)
        return 0;
    uint64_t answer = 0;
    GUARD_TRY {
        answer = ((WidgetStepFn)global_promptParts.firstChild)(container);
    } GUARD_EXCEPT { answer = 0; }
    return answer;
}

static uint64_t nextSiblingObjectOf(uint64_t child) {
    if (child < SMALLEST_POINTER || !global_promptParts.nextSibling) return 0;
    uint64_t answer = 0;
    GUARD_TRY {
        answer = ((WidgetStepFn)global_promptParts.nextSibling)(child);
    } GUARD_EXCEPT { answer = 0; }
    return answer;
}

/* The interface's root widget, or 0. */
static uint64_t hudRoot(void) {
    if (!global_promptParts.hudHandle) return 0;
    return objectOfHandle(global_promptParts.hudHandle,
                          global_promptParts.hudProcessId);
}

/* The method in a widget's table at slot, or 0. */
static uint64_t methodOf(uint64_t widget, unsigned slot) {
    uint64_t table = 0, m = 0;
    if (!readQword(widget, &table) || !table) return 0;
    if (!readQword(table + slot, &m) || !inCode(m)) return 0;
    return m;
}

/* A widget's class table, where it lies in the game; else 0. */
static uint64_t classTableOf(uint64_t widget) {
    uint64_t table = 0;
    if (!readQword(widget, &table) || !table || !inModule(table)) return 0;
    return table;
}

/* Whether the engine would draw a widget: 1, 0, or -1 when it cannot be
   asked. */
static int widgetDrawn(uint64_t widget) {
    const uint64_t method = methodOf(widget, global_promptParts.isVisibleSlot);
    if (!method) return -1;
    int answer = -1;
    GUARD_TRY { answer = ((WidgetAnswerFn)method)(widget) ? 1 : 0; }
    GUARD_EXCEPT { answer = -1; }
    return answer;
}

/* The widget file a widget was made from, or 0. */
static uint64_t widgetFileOf(uint64_t widget) {
    uint64_t file = 0;
    readQword(widget + global_promptParts.widgetFileId, &file);
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
    const PromptParts* p = &global_promptParts;
    unsigned own = 0;
    if (!readDword(live + p->ownCategories, &own) || own == p->titleCards)
        return;
    GUARD_TRY {
        ((AssignCategoryFn)p->assignCategory)(live, p->titleCards);
    } GUARD_EXCEPT {}
    unsigned now = 0;
    readDword(live + p->ownCategories, &now);
    if (now != p->titleCards)
        writeDword(live + p->ownCategories, p->titleCards);
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
    const uint64_t method =
        widget ? methodOf(widget, global_promptParts.setPropertySlot) : 0;
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

/* What the key icon writer writes for the game's code for a key, large or
   not, into out; empty when it writes nothing or faults. */
static void keyIconPath(uint8_t code, int large, char* out, size_t room) {
    out[0] = 0;
    if (!global_promptParts.keyIconText) return;
    IconBuilder builder;
    memset(&builder, 0, sizeof(builder));
    builder.type = global_promptParts.builderClass;
    builder.capacity = 0x100;
    int ok = 1;
    GUARD_TRY {
        ((KeyIconTextFn)global_promptParts.keyIconText)(
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
    /* Where there is no large picture the key icon writer falls back to the
       ordinary one's .svg, or names a .png that may not be there. */
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
        const uint64_t table = classTableOf(part);
        if (!table) continue;
        if (table == global_promptParts.imageClass) {
            if (!global_promptImage) global_promptImage = part;
            else if (!global_promptProbe) global_promptProbe = part;
        } else if (table == global_promptParts.textClass && !global_promptLabel)
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
    const uint64_t rect = global_promptProbe + global_promptParts.layoutRect;
    float width = 0.0f, height = 0.0f;
    if (!readFloat(rect + 8, &width) || !readFloat(rect + 12, &height))
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

/* Part II's offer, described where it is defined (The Last of Us Part II,
   below). */
static int partTwoOffer(uint64_t* node, uint64_t* skipTo, uint64_t* checkpoint);

/* 1 while a skip is offered, with where it goes in *target and the prompt's
   text in *label; else 0. In Part I from resolveSkip and labelFor, the target
   the task's name; in Part II from partTwoOffer, the target the task's node. */
static int promptOffer(uint64_t* target, const char** label) {
    *target = 0;
    *label = "";
    if (global_partTwo) {
        uint64_t skipTo = 0, checkpoint = 0;
        if (partTwoOffer(target, &skipTo, &checkpoint) <= 0) return 0;
        *label = labelFor(0, 1);
        return 1;
    }
    uint64_t scene = 0, checkpoint = 0, countedFrom = 0;
    int cinematic = 0, how = 0;
    if (!resolveSkip(&scene, target, &checkpoint, &cinematic, &how,
                     &countedFrom))
        return 0;
    *label = labelFor(scene, cinematic);
    return 1;
}

/*
    Shows the prompt while a skip is offered, with the scene's text. An offer
    with another target or text than the one shown fades it in; one that comes
    back within PROMPT_OFFER_GRACE_MS of being lost is the same offer, so a
    prompt that had faded out stays gone. No offer hides it at once.
*/
static void updatePrompt(uint64_t prompt) {
    uint64_t target = 0;
    const char* label = "";
    const int offered = promptOffer(&target, &label);
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
        w = ((GetOrCreateChildFn)global_promptParts.getOrCreate)(
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
    if (!global_promptPartsFound || !promptOn()) return;
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
    if (!global_promptPartsFound || !promptOn() || !global_promptLive
        || !promptShowing())
        return;
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
    if (!global_promptPartsFound) return;
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

/* ------------------------------------------- The Last of Us Part II */

/*
    In The Last of Us Part II the key makes only the game's own Skip
    Cinematic, the way Yes in the pause menu's Skip Cinematic box makes it.
    Menu2::RootGame::EventHandlerMessageBox, on Yes in that box:

      1. asks SkipTarget(the cinematic manager, &skip to, &checkpoint) for
         the task the skip goes to, and stops when there is none;
      2. where the selection mode (a dword) is 1, an encounter selection, or
         2, a cinematic selection, asks selectionEnds(the task manager's
         first graph, the target, the checkpoint), and where that answers
         true ends the selection instead of skipping;
      3. asks speedrunOn(the speedrun object) and speedrunEnds(the speedrun
         object, the target), and where both answer true ends the speedrun
         instead of skipping;
      4. otherwise broadcasts the state script event cinematic-skip through
         the state script manager, with the skip to and the checkpoint as
         names and two empty arguments, and sets the menu root's skip
         countdown to 2.

    Menu2::RootGame::PostRenderUpdate reads the countdown once a call (r14
    the root), -1 standing for none: above 0 it counts it down; at 0 it sets
    -1, asks SkipTarget again and plays the target through
    GameTaskSubnode::SkipTo at its checkpoint, default where there is none.
    It plays nothing where SkipTarget then answers none, where SkipTo
    refuses, or where GameInfo's slot 0x370 answers true and 01C5E370, asked
    with 14 of what 01B31E30 gives for the cinematic manager, answers nonzero
    (rvas of tlou-ii.exe). The frame's job GameFramePostRenderUpdate calls it
    once a frame through the widget file manager (01BF1C10, while that
    manager's byte at +0x1010 is set), earlier in the job than
    ScriptManager::ListenerUpdate, the tick.

    BroadcastEvent queues the event while GameInfo entry 0x13's bytes +6 and
    +0x34 are clear (01449D40); the job delivers the queue (01572C10) after
    PostRenderUpdate and before the tick, with +6 set. Byte +6, deliver at
    once, is set through the entry's setter (0144C200), which takes the
    entry's lock and answers the old value. While it is set, BroadcastEvent
    hands the event to every subscribed process within the call, and a state
    script runs its handler for the event there and then.

    The pause menu shows its Skip Cinematic row (optional-skip-cinematic)
    while a byte of the guitar free play object is clear and SkipOffered
    answers true; the byte is set while guitar free play runs.

    A press of the key while the pause menu would show that row, with the
    game window in front, no in-game menu up and no skip counting down,
    makes 1 to 3 with the game's own functions and sets the countdown to 2,
    and makes nothing where 2 or 3 would end a selection or a speedrun.
    GetAsyncKeyState answers for the whole desktop, so the key counts only
    while the window in front is the game's.

    The skip is decided where PostRenderUpdate reads the countdown, which is
    hooked, and nowhere else. When that read will find 0 in the root the key
    set the countdown in, the row's test and SkipTarget are asked again.
    Where the row would show and SkipTarget answers the target, skip to and
    checkpoint it answered at the press, cinematic-skip is broadcast as in 4
    and the countdown is left at 0, so the same call goes on to SkipTo;
    otherwise the countdown is set to -1 and nothing is broadcast, so the
    game plays nothing. The broadcast is delivered at once, with entry 0x13's
    deliver-at-once byte set through the game's setter and restored after,
    the entry asked of the getter each time, as the game's drain does, so
    the state scripts handle it before that SkipTo. A countdown the game
    has not counted out within PART_TWO_WAIT_TICKS ticks of the tick is set
    back to -1, with one line in the log, and nothing is broadcast. Once the
    menu root's getter no longer gives the root the countdown was set in, or
    the countdown cannot be read, the skip can only be withdrawn: a read
    that finds 0 in that root sets -1.

    No part has a fixed address. At Init each is found in the game's code:

      SkipOffered            by its opening, which stands once in the code;
                             its call at +0x20 is SkipTarget and its lea at
                             +0x28 the task manager
      the row's test         by the pause menu's test for the row, which
                             stands once in the code: the byte, then the
                             getter and SkipOffered, its jumps checked to
                             land on the answers hidden and shown
      the Yes branch         in EventHandlerMessageBox (by its signature),
                             four pieces each standing once there, joined by
                             their own jumps: the head (the cinematic
                             manager's getter, SkipTarget, the selection
                             mode, the task manager, the first graph and
                             selectionEnds), the second mode test, the
                             speedrun tests, and the broadcast with the
                             countdown's place in the menu root
      the countdown          in PostRenderUpdate (by its signature): its
                             count, which opens with the read of the
                             countdown (mov eax, [r14 + the countdown's
                             place], seven bytes, hooked) and ends in the
                             getter and SkipTarget, and its call of
                             GameTaskSubnode::SkipTo (by its signature)
      the menu root          the function that hides the interface for a
                             cinematic, which stands once in the code: it
                             calls the menu root's getter (Menu2::RootGame,
                             or 0) and the root's slot (root, 0), then the
                             cinematic manager's getter. The slot answers 1
                             while the root's page stack has a top page
                             whose byte +0x4E5 is set, or whose byte +0x4E6
                             is set and whose slot 0x2F0 answers true, and
                             which is none of menu/page-cinema-control,
                             pc/menu/page-photo-mode-pc,
                             pc/menu/page-eye-direction-pc and
                             pc/menu/page-color-picker-pc; else 0
      the delivery at once   the job's drain of the event queue, which
                             stands once in the code: GameInfo's entry
                             getter and the deliver-at-once setter, each
                             called twice, and the queue; then
                             SsManager::BroadcastEvent (by its signature),
                             the function BroadcastEvent calls, whose choice
                             between the queue and delivery at once stands
                             once in its opening: the same getter, the queue
                             test (which reads bytes +6 and +0x34) and the
                             same queue

    Each function called opens with the bytes expected, except the root's
    in-menu method, which is read from the root's class table and checked
    only to lie in the game's code; an address named in more than one place
    is the same in each; the cinematic manager's pointer, the mode, the task
    manager, the speedrun object's pointer, the state script manager, the
    event queue and the row's byte lie in the game's data. The countdown is written only in a
    menu root whose class table holds that PostRenderUpdate: at the press,
    in the root the getter gives; at the read, in the root that call
    updates; and after PART_TWO_WAIT_TICKS ticks, only while the getter
    still gives that root. Where a part is not found, or the loader refuses
    either hook, the key makes no skips, and one line in the log says which.
    Another plugin may have hooked SkipOffered at +0x16 and the Yes branch
    just after SkipTarget, so the patterns pass over those bytes.
*/

#define PART_TWO_PATTERN_BYTES   128   /* the longest pattern */
#define PART_TWO_COUNTDOWN       2     /* the countdown Yes sets */
#define PART_TWO_NO_COUNTDOWN    0xFFFFFFFFu /* -1: no skip counting down */
#define PART_TWO_WAIT_TICKS      120   /* ticks the countdown may take before
                                          the skip is withdrawn */
#define COUNTDOWN_READ_BYTES     "41 8B 86"   /* mov eax, [r14 + disp32] */
#define PART_TWO_ROOT_SLOTS      256   /* the root's class table slots searched
                                          for PostRenderUpdate */
#define PART_TWO_LARGEST_FIELD   0x1000000u /* the countdown's place in the root
                                          is below this */
#define MODE_ENCOUNTER_SELECTION 1
#define MODE_CINEMATIC_SELECTION 2

static const char* const messageBoxSignature =
    "void __cdecl Menu2::RootGame::EventHandlerMessageBox(class Event &)";
static const char* const rootUpdateSignature =
    "void __cdecl Menu2::RootGame::PostRenderUpdate(void)";

/* SkipOffered's opening. Each _AT below is where a four byte displacement or
   a field stands, from the start of the pattern it follows. */
static const char* const skipOfferedPattern =
    "48 89 5C 24 08 57 48 83 EC 20 33 C0 4C 8D 44 24 38 48 8D 54 24 40 "
    "?? ?? ?? ?? ?? 48 89 44 24 38 E8 ?? ?? ?? ?? 45 33 C0 48 8D 0D ?? ?? ?? "
    "?? 33 D2 48 8B F8 E8";
#define OFFERED_TARGET_AT        0x21   /* call SkipTarget */
#define OFFERED_TASK_MANAGER_AT  0x2B   /* lea rcx, [the task manager] */

/* The Yes branch's head: the getter, SkipTarget(rcx, &[rbp-30h] skip to,
   &[rbp-20h] checkpoint), then mode 1's test. */
static const char* const yesHeadPattern =
    "33 F6 48 89 75 D0 48 89 75 E0 E8 ?? ?? ?? ?? 4C 8D 45 E0 48 8D 55 D0 "
    "48 8B C8 E8 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 8B 05 ?? ?? "
    "?? ?? 83 F8 01 75 ?? 48 8B 5D E0 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C "
    "8B C3 49 8B D6 48 8B C8 E8 ?? ?? ?? ?? 84 C0";
#define YES_GETTER_AT            0x0B
#define YES_TARGET_AT            0x1B
#define YES_MODE_AT              0x2D
#define YES_MODE_JUMP_AT         0x35   /* jne to the second mode test, a byte */
#define YES_TASK_MANAGER_AT      0x3D
#define YES_FIRST_GRAPH_AT       0x42
#define YES_SELECTION_ENDS_AT    0x50

/* The second mode test, where the head's jump lands at its cmp. */
static const char* const secondModePattern =
    "8B 05 ?? ?? ?? ?? 83 F8 02 75 ?? 48 8B 5D E0 48 8D 0D ?? ?? ?? ?? E8 ?? "
    "?? ?? ?? 4C 8B C3 49 8B D6 48 8B C8 E8 ?? ?? ?? ?? 84 C0";
#define MODE_MODE_AT             0x02
#define MODE_COMPARE             0x06   /* cmp eax, 2 */
#define MODE_JUMP_AT             0x0A   /* jne to the speedrun tests, a byte */
#define MODE_TASK_MANAGER_AT     0x12
#define MODE_FIRST_GRAPH_AT      0x17
#define MODE_SELECTION_ENDS_AT   0x25

/* The speedrun tests: speedrunOn([the object]), then speedrunEnds([the
   object], the target), each jumping to the broadcast when it answers 0. */
static const char* const speedrunPattern =
    "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 49 8B D6 48 "
    "8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? C6 87 ?? ?? ?? "
    "?? 01";
#define SPEEDRUN_OBJECT_AT       0x03
#define SPEEDRUN_ON_AT           0x08
#define SPEEDRUN_ON_JUMP_AT      0x10
#define SPEEDRUN_OBJECT_AGAIN_AT 0x1A
#define SPEEDRUN_ENDS_AT         0x1F
#define SPEEDRUN_ENDS_JUMP_AT    0x27

/* The broadcast: [rbp-8] {skip to, name}, [rsp+50h] {checkpoint, name},
   [rsp+40h] and [rsp+30h] empty, the event cinematic-skip, then the
   countdown set to 2 in the root (rdi). */
static const char* const broadcastPattern =
    "89 74 24 3C 48 89 74 24 30 89 74 24 4C 48 89 74 24 40 C7 44 24 5C 07 00 "
    "00 00 48 8B 45 E0 48 89 44 24 50 C7 45 04 07 00 00 00 48 8B 45 D0 48 89 "
    "45 F8 48 BA E6 C5 36 D8 E8 E3 B2 05 48 8D 44 24 30 48 89 44 24 28 48 8D "
    "44 24 40 48 89 44 24 20 4C 8D 4C 24 50 4C 8D 45 F8 48 8D 0D ?? ?? ?? ?? "
    "E8 ?? ?? ?? ?? BA 02 00 00 00 89 97 ?? ?? ?? ??";
#define BROADCAST_MANAGER_AT     0x5C
#define BROADCAST_CALL_AT        0x61
#define BROADCAST_COUNTDOWN_AT   0x6C   /* the countdown's place in the root */

/* PostRenderUpdate's count (r14 the root): at 0 it sets -1 and asks the
   getter and SkipTarget. */
static const char* const countdownPattern =
    "41 8B 86 ?? ?? ?? ?? 85 C0 0F 85 ?? ?? ?? ?? 41 C7 86 ?? ?? ?? ?? FF FF "
    "FF FF 48 89 BD ?? ?? ?? ?? 48 89 BD ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8D 85 "
    "?? ?? ?? ?? 48 8D 95 ?? ?? ?? ?? 48 8B C8 E8";
#define COUNTDOWN_READ_AT        0x03
#define COUNTDOWN_IDLE_AT        0x12
#define COUNTDOWN_GETTER_AT      0x29
#define COUNTDOWN_TARGET_AT      0x3F

/* PostRenderUpdate's call of GameTaskSubnode::SkipTo(the target, the
   checkpoint or default). */
static const char* const skipToCallPattern =
    "48 8B 95 ?? ?? ?? ?? 48 B8 FE C5 20 86 16 A5 AD EB 48 85 D2 48 0F 44 D0 "
    "48 89 95 ?? ?? ?? ?? 48 8B CE E8 ?? ?? ?? ?? 84 C0";
#define SKIP_TO_CALL_AT          0x23

/* The function that hides the interface for a cinematic, to its category
   mask: the player, the menu root, the root's in-menu slot, the cinematic
   manager. */
static const char* const hideInterfacePattern =
    "48 83 EC 28 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 D2 48 8B 08 4C 8B 81 ?? ?? "
    "?? ?? 48 8B C8 41 FF D0 84 C0 75 ?? E8 ?? ?? ?? ?? 45 33 C0 33 D2 48 8B "
    "C8 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? "
    "48 8B 01 FF 50 70 84 C0 74 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 78 84 "
    "C0 75 ?? 33 D2 8D 4A 17 E8 ?? ?? ?? ?? BA D5 EF BF FF";
#define HIDE_MENU_ROOT_AT        0x0A
#define HIDE_IN_MENU_SLOT_AT     0x16
#define HIDE_GETTER_AT           0x25

/* The pause menu's test for its Skip Cinematic row: cmp byte [the byte], 0;
   jne hidden; the getter; SkipOffered; jne shown; jmp hidden. */
static const char* const skipRowPattern =
    "80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? "
    "?? ?? 84 C0 0F 85 ?? ?? ?? ?? E9";
#define ROW_BYTE_AT              0x02   /* the cmp ends one byte after it */
#define ROW_HIDDEN_JUMP_AT       0x09
#define ROW_GETTER_AT            0x0E
#define ROW_OFFERED_AT           0x16
#define ROW_SHOWN_JUMP_AT        0x1E
#define ROW_HIDDEN_AGAIN_AT      0x23
static const char* const rowHiddenAnswer = "B0 01";   /* mov al, 1 */
static const char* const rowShownAnswer = "32 C0";    /* xor al, al */

/* The openings of the functions called. */
static const char* const skipTargetOpening =
    "4C 8B DC 56 57 41 54 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 "
    "24 38";
static const char* const getterOpening =
    "48 83 EC 38 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 85";
#define GETTER_POINTER_AT        0x07   /* mov rax, [the cinematic manager] */
static const char* const firstGraphOpening =
    "48 81 C1 B0 00 00 00 48 8B 01 48 3B C1 75 03 33 C0 C3 48 8B 40 10 C3";
static const char* const selectionEndsOpening =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B E9 49 "
    "8B D8";
static const char* const speedrunOnOpening =
    "48 83 EC 28 33 D2 33 C9 E8 ?? ?? ?? ?? 8B 90 ?? ?? ?? ?? 83 EA 01";
static const char* const speedrunEndsOpening =
    "40 53 48 83 EC 20 48 8B DA 33 C9 33 D2 E8 ?? ?? ?? ?? 48 8D 4C 24 38 8B "
    "90";
static const char* const broadcastOpening =
    "48 89 5C 24 08 48 89 6C 24 10 56 57 41 54 41 56 41 57 48 81 EC 80 01 00 "
    "00";
static const char* const menuRootOpening =
    "48 83 EC 28 E8 ?? ?? ?? ?? 4C 8B C8 33 C0 4D 85 C9 74";

/* The delivery of a state script event at once. */
#define PART_TWO_EVENT_ENTRY     0x13   /* GameInfo's table entry for state
                                           script events */
#define PART_TWO_DISPATCH_BYTES  0x100  /* bytes of SsManager::BroadcastEvent
                                           searched from its start */
static const char* const dispatchSignature =
    "void __cdecl SsManager::BroadcastEvent(class Process *,class Event &)";

/* The job's drain of the event queue: the entry getter(0x13), the setter
   (entry, 1), the queue delivered, then the getter(0x13) and the setter
   (entry, 0) again, the last by a jump. */
static const char* const drainPattern =
    "48 83 EC 28 33 D2 8D 4A 13 E8 ?? ?? ?? ?? B2 01 48 8B C8 E8 ?? ?? ?? ?? "
    "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 D2 8D 4A 13 E8 ?? ?? ?? ?? 33 D2 "
    "48 8B C8 48 83 C4 28 E9 ?? ?? ?? ??";
#define DRAIN_ENTRY_AT           0x0A
#define DRAIN_SET_AT             0x14
#define DRAIN_QUEUE_AT           0x1B   /* lea rcx, [the event queue] */
#define DRAIN_ENTRY_AGAIN_AT     0x2A
#define DRAIN_SET_AGAIN_AT       0x38

/* BroadcastEvent's call of SsManager::BroadcastEvent(the script manager, no
   process, the event), at BROADCAST_DISPATCH_AT in BroadcastEvent. */
static const char* const broadcastDispatchCall =
    "4C 8D 44 24 40 33 D2 48 8B CF E8";
#define BROADCAST_DISPATCH_AT    0xE3
#define DISPATCH_CALL_AT         0x0B

/* SsManager::BroadcastEvent's choice: the entry getter(0x13), the queue
   test, and where it answers true the event put into the queue. */
static const char* const dispatchQueuePattern =
    "33 D2 8D 4A 13 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 84 C0 74 ?? 41 80 "
    "8E 2A 01 00 00 02 48 89 5C 24 40 89 5C 24 48 C5 F8 10 44 24 40 C5 F9 7F "
    "44 24 40 4C 8D 44 24 40 49 8B D6 48 8D 0D ?? ?? ?? ?? E8";
#define DISPATCH_ENTRY_AT        0x06
#define DISPATCH_TEST_AT         0x0E
#define DISPATCH_QUEUE_AT        0x3E

/* The entry getter, (index) -> the entry; the setter, (entry, value) -> the
   old value, which stores the value in the entry's byte +6 under the
   entry's lock at +0x180; the queue test, true while +6 and +0x34 are
   clear. */
static const char* const eventEntryOpening =
    "8B C1 48 8D 0D ?? ?? ?? ?? 48 8B 04 C1 C3";
static const char* const deliverAtOnceOpening =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 0F B6 F2 48 "
    "8B F9 4C 8D 0D ?? ?? ?? ?? 41 B8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 81 "
    "C1 80 01 00 00 E8 ?? ?? ?? ?? 0F B6 5F 06 40 88 77 06";
static const char* const queueTestOpening =
    "80 79 06 00 75 ?? 80 79 34 00 75 ?? B0 01 C3";

/* The bytes of the entry the setter reads and writes: from its
   deliver-at-once byte (+6) to the end of its lock (+0x180: the holder, a
   qword, then at +0x190 the count of holds, a dword). */
static const uint64_t eventEntryTouchedFrom = 0x06;
static const uint64_t eventEntryTouchedEnd = 0x194;

typedef uint64_t (*EventEntryFn)(unsigned);
typedef unsigned char (*DeliverAtOnceFn)(uint64_t, unsigned char);
typedef uint64_t (*FirstGraphFn)(uint64_t);
typedef unsigned char (*SelectionEndsFn)(uint64_t, uint64_t, uint64_t);
typedef unsigned char (*SpeedrunOnFn)(uint64_t);
typedef unsigned char (*SpeedrunEndsFn)(uint64_t, uint64_t);
typedef uint64_t (*MenuRootFn)(void);

/* What the key uses in Part II, as findPartTwo finds it. */
struct PartTwoParts {
    uint64_t skipOffered;     /* (cinematic manager) -> Skip Cinematic is offered */
    uint64_t skipTarget;      /* (cinematic manager, &skip to, &checkpoint) -> the target task */
    uint64_t managerPointer;  /* -> the cinematic manager */
    uint64_t taskManager;     /* the task manager */
    uint64_t modeWord;        /* the selection mode, a dword */
    uint64_t firstGraph;      /* (task manager) -> its first task graph, or 0 */
    uint64_t selectionEnds;   /* (graph, target, checkpoint) -> the target ends the selection */
    uint64_t speedrunPointer; /* -> the speedrun object */
    uint64_t speedrunOn;      /* (speedrun object) -> GameInfo's speed-run-toggle is 2 or 3 */
    uint64_t speedrunEnds;    /* (speedrun object, target) -> the target ends the speedrun */
    uint64_t scriptManager;   /* the state script manager */
    uint64_t broadcast;       /* (script manager, event, four BoxedValue*) */
    uint64_t menuRoot;        /* () -> the menu root, Menu2::RootGame, or 0 */
    uint64_t rootUpdate;      /* Menu2::RootGame::PostRenderUpdate */
    uint64_t rowHidden;       /* a byte: while set the pause menu hides its Skip Cinematic row */
    uint64_t countdownRead;   /* in PostRenderUpdate, mov eax, [r14 + countdown]: its read of the countdown, hooked */
    uint64_t eventEntry;      /* (index) -> GameInfo's table entry; PART_TWO_EVENT_ENTRY for state script events */
    uint64_t deliverAtOnce;   /* (entry, value) -> the old value: sets the entry's deliver-at-once byte */
    unsigned inMenuSlot;      /* the root's (root, 0) -> an in-game menu is up */
    unsigned countdown;       /* the root's skip countdown, an int */
};

static PartTwoParts global_partTwoParts;
static uint64_t global_partTwoRootTable = 0;   /* the root's class table, once checked */
/* The skip the key started and the game has not yet decided: the root its
   countdown was set in, 0 for none. Whoever ends the wait (the read of the
   countdown, or the tick) takes it by exchanging it for 0. */
static volatile LONG64 global_partTwoWaitRoot = 0;
/* 1 once the getter no longer gave that root, or its countdown could not be
   read: the skip can then only be withdrawn, by the read in that root. */
static volatile LONG global_partTwoWaitAbandoned = 0;
static uint64_t global_partTwoWaitNode = 0;    /* SkipTarget's target at the press */
static uint64_t global_partTwoWaitSkipTo = 0;  /* its skip to */
static uint64_t global_partTwoWaitCheckpoint = 0;  /* and its checkpoint */
static int      global_partTwoWaitTicks = 0;
static int      global_partTwoSaid = 0;        /* the failures logged, a bit each */

#define SAID_ROOT        1
#define SAID_OFFER       2
#define SAID_TESTS       4
#define SAID_BROADCAST   8
#define SAID_COUNTDOWN   16
#define SAID_NOT_COUNTED 32
#define SAID_MENU        64
#define SAID_RESTORE     128

/* A pattern of code: hexadecimal bytes separated by spaces, ?? for any byte,
   the first byte fixed. Its bytes, and in care which of them count; its
   length, 0 when the text is not such a pattern. */
static int parsePattern(const char* text, uint8_t* bytes, uint8_t* care,
                        int room) {
    int length = 0;
    for (const char* at = text; *at; ) {
        while (*at == ' ') ++at;
        if (!*at) break;
        if (length >= room) return 0;
        if (at[0] == '?' && at[1] == '?') {
            bytes[length] = 0;
            care[length++] = 0;
            at += 2;
            continue;
        }
        char* stop = nullptr;
        const unsigned long value = strtoul(at, &stop, 16);
        if (!stop || stop == at || value > 0xFF) return 0;
        bytes[length] = (uint8_t)value;
        care[length++] = 1;
        at = stop;
    }
    return length && care[0] ? length : 0;
}

static int bytesMatch(const uint8_t* at, const uint8_t* bytes,
                      const uint8_t* care, int length) {
    for (int i = 0; i < length; ++i)
        if (care[i] && at[i] != bytes[i]) return 0;
    return 1;
}

/* Whether the game's code at address holds the pattern. */
static int codeMatches(uint64_t address, const char* pattern) {
    uint8_t bytes[PART_TWO_PATTERN_BYTES], care[PART_TWO_PATTERN_BYTES];
    const int length = parsePattern(pattern, bytes, care,
                                    PART_TWO_PATTERN_BYTES);
    if (!length || !inCode(address)
        || !inCode(address + (uint64_t)length - 1))
        return 0;
    int matched = 0;
    GUARD_TRY {
        matched = bytesMatch((const uint8_t*)(uintptr_t)address, bytes, care,
                             length);
    } GUARD_EXCEPT { matched = 0; }
    return matched;
}

/* The places from first, for length bytes, holding the pattern, the first
   room of them written to found: their count, room + 1 when there are more,
   and 0 when there are none or the text is not a pattern. */
static int placesIn(uint64_t first, uint64_t length, const char* pattern,
                    uint64_t* found, int room) {
    uint8_t bytes[PART_TWO_PATTERN_BYTES], care[PART_TWO_PATTERN_BYTES];
    const int size = parsePattern(pattern, bytes, care, PART_TWO_PATTERN_BYTES);
    if (!size || !first || length < (uint64_t)size) return 0;
    int count = 0;
    GUARD_TRY {
        const uint8_t* at = (const uint8_t*)(uintptr_t)first;
        const uint8_t* last = (const uint8_t*)(uintptr_t)(first + length
                                                          - (uint64_t)size);
        while (at <= last && count <= room) {
            const uint8_t* hit = (const uint8_t*)memchr(
                at, bytes[0], (size_t)(last - at) + 1);
            if (!hit) break;
            if (bytesMatch(hit, bytes, care, size)) {
                if (count < room) found[count] = (uint64_t)(uintptr_t)hit;
                ++count;
            }
            at = hit + 1;
        }
    } GUARD_EXCEPT { count = 0; }
    return count;
}

/* The one place from first, for length bytes, holding the pattern; 0 when
   there is not exactly one. */
static uint64_t onlyPlaceIn(uint64_t first, uint64_t length,
                            const char* pattern) {
    uint64_t found = 0;
    return placesIn(first, length, pattern, &found, 1) == 1 ? found : 0;
}

/* The address a four byte displacement at at names, counted from the end of
   its instruction, which ends with it; 0 when it cannot be read. */
static uint64_t displacementTarget(uint64_t at) {
    unsigned int word = 0;
    if (!at || !readDword(at, &word)) return 0;
    return at + 4 + (uint64_t)(int64_t)(int32_t)word;
}

/* The address a one byte jump displacement at at names. */
static uint64_t shortJumpTarget(uint64_t at) {
    unsigned char byte = 0;
    if (!at || !readByte(at, &byte)) return 0;
    return at + 1 + (uint64_t)(int64_t)(int8_t)byte;
}

/* An address in the game's data. */
static int inData(uint64_t address) {
    return inModule(address) && !inCode(address);
}

/* The function a signature names: its entry, with the length of its first
   part in *length; 0 when it is not found. */
static uint64_t functionNamed(const char* name, const char* signature,
                              uint64_t* length) {
    *length = 0;
    const uint64_t found = loaderFindFunction(name, signature);
    uint64_t entry = 0;
    GUARD_TRY { entry = found ? entryOfFunction(found) : 0; }
    GUARD_EXCEPT { entry = 0; }
    if (!entry) return 0;
    DWORD64 imageBase = 0;
    const PRUNTIME_FUNCTION part =
        RtlLookupFunctionEntry((DWORD64)entry, &imageBase, nullptr);
    if (!part || imageBase + part->BeginAddress != entry
        || part->EndAddress <= part->BeginAddress)
        return 0;
    *length = part->EndAddress - part->BeginAddress;
    return entry;
}

static int partTwoNotFound(const char* what) {
    writeLog("SKIP %s was not found in the game's code as the Scene Skipper "
             "expects it, so the skip key makes no skips", what);
    return 0;
}

/* Every part of Part II's skip, found and checked into global_partTwoParts;
   1 when all are, else 0 with one line in the log. */
static int findPartTwo(void) {
    static const char* const yesBranch =
        "the pause menu's Yes for Skip Cinematic, in "
        "Menu2::RootGame::EventHandlerMessageBox";
    PartTwoParts p;
    memset(&p, 0, sizeof(p));

    const uint64_t offered = onlyPlaceIn(global_textBase, global_textSize,
                                         skipOfferedPattern);
    if (!offered)
        return partTwoNotFound("the game's Skip Cinematic test (SkipOffered)");
    p.skipOffered = offered;
    p.skipTarget = displacementTarget(offered + OFFERED_TARGET_AT);
    p.taskManager = displacementTarget(offered + OFFERED_TASK_MANAGER_AT);
    if (!codeMatches(p.skipTarget, skipTargetOpening))
        return partTwoNotFound("the game's Skip Cinematic target (SkipTarget)");

    uint64_t length = 0;
    const uint64_t handler = functionNamed(
        "Menu2::RootGame::EventHandlerMessageBox", messageBoxSignature,
        &length);
    const uint64_t head = onlyPlaceIn(handler, length, yesHeadPattern);
    const uint64_t mode = onlyPlaceIn(handler, length, secondModePattern);
    const uint64_t speedrun = onlyPlaceIn(handler, length, speedrunPattern);
    const uint64_t broadcast = onlyPlaceIn(handler, length, broadcastPattern);
    if (!head || !mode || !speedrun || !broadcast
        || shortJumpTarget(head + YES_MODE_JUMP_AT) != mode + MODE_COMPARE
        || shortJumpTarget(mode + MODE_JUMP_AT) != speedrun
        || displacementTarget(speedrun + SPEEDRUN_ON_JUMP_AT) != broadcast
        || displacementTarget(speedrun + SPEEDRUN_ENDS_JUMP_AT) != broadcast)
        return partTwoNotFound(yesBranch);
    const uint64_t getter = displacementTarget(head + YES_GETTER_AT);
    p.modeWord = displacementTarget(head + YES_MODE_AT);
    p.firstGraph = displacementTarget(head + YES_FIRST_GRAPH_AT);
    p.selectionEnds = displacementTarget(head + YES_SELECTION_ENDS_AT);
    p.speedrunPointer = displacementTarget(speedrun + SPEEDRUN_OBJECT_AT);
    p.speedrunOn = displacementTarget(speedrun + SPEEDRUN_ON_AT);
    p.speedrunEnds = displacementTarget(speedrun + SPEEDRUN_ENDS_AT);
    p.scriptManager = displacementTarget(broadcast + BROADCAST_MANAGER_AT);
    p.broadcast = displacementTarget(broadcast + BROADCAST_CALL_AT);
    if (!readDword(broadcast + BROADCAST_COUNTDOWN_AT, &p.countdown)
        || displacementTarget(head + YES_TARGET_AT) != p.skipTarget
        || displacementTarget(head + YES_TASK_MANAGER_AT) != p.taskManager
        || displacementTarget(mode + MODE_MODE_AT) != p.modeWord
        || displacementTarget(mode + MODE_TASK_MANAGER_AT) != p.taskManager
        || displacementTarget(mode + MODE_FIRST_GRAPH_AT) != p.firstGraph
        || displacementTarget(mode + MODE_SELECTION_ENDS_AT) != p.selectionEnds
        || displacementTarget(speedrun + SPEEDRUN_OBJECT_AGAIN_AT)
               != p.speedrunPointer)
        return partTwoNotFound(yesBranch);
    if (!codeMatches(getter, getterOpening))
        return partTwoNotFound("the cinematic manager's getter");
    p.managerPointer = displacementTarget(getter + GETTER_POINTER_AT);

    const uint64_t row = onlyPlaceIn(global_textBase, global_textSize,
                                     skipRowPattern);
    const uint64_t rowByte = row ? displacementTarget(row + ROW_BYTE_AT) : 0;
    const uint64_t hidden = row ? displacementTarget(row + ROW_HIDDEN_JUMP_AT) : 0;
    p.rowHidden = rowByte ? rowByte + 1 : 0;
    if (!row || displacementTarget(row + ROW_GETTER_AT) != getter
        || displacementTarget(row + ROW_OFFERED_AT) != p.skipOffered
        || displacementTarget(row + ROW_HIDDEN_AGAIN_AT) != hidden
        || !codeMatches(hidden, rowHiddenAnswer)
        || !codeMatches(displacementTarget(row + ROW_SHOWN_JUMP_AT),
                        rowShownAnswer)
        || !inData(p.rowHidden))
        return partTwoNotFound("the pause menu's test for its Skip Cinematic "
                               "row");
    if (!codeMatches(p.firstGraph, firstGraphOpening)
        || !codeMatches(p.selectionEnds, selectionEndsOpening))
        return partTwoNotFound("the game's test for the end of an encounter "
                               "or cinematic selection");
    if (!codeMatches(p.speedrunOn, speedrunOnOpening)
        || !codeMatches(p.speedrunEnds, speedrunEndsOpening))
        return partTwoNotFound("the game's test for the end of a speedrun");
    if (!codeMatches(p.broadcast, broadcastOpening))
        return partTwoNotFound("the game's broadcast of a state script event");

    static const char* const atOnce =
        "the game's delivery of a state script event at once";
    const uint64_t drain = onlyPlaceIn(global_textBase, global_textSize,
                                       drainPattern);
    if (!drain) return partTwoNotFound(atOnce);
    p.eventEntry = displacementTarget(drain + DRAIN_ENTRY_AT);
    p.deliverAtOnce = displacementTarget(drain + DRAIN_SET_AT);
    const uint64_t queue = displacementTarget(drain + DRAIN_QUEUE_AT);
    uint64_t dispatchLength = 0;
    const uint64_t dispatch = functionNamed("SsManager::BroadcastEvent",
                                            dispatchSignature, &dispatchLength);
    const uint64_t choice =
        dispatch && codeMatches(p.broadcast + BROADCAST_DISPATCH_AT,
                                broadcastDispatchCall)
        && displacementTarget(p.broadcast + BROADCAST_DISPATCH_AT
                              + DISPATCH_CALL_AT) == dispatch
            ? onlyPlaceIn(dispatch, PART_TWO_DISPATCH_BYTES, dispatchQueuePattern)
            : 0;
    if (!choice
        || displacementTarget(drain + DRAIN_ENTRY_AGAIN_AT) != p.eventEntry
        || displacementTarget(drain + DRAIN_SET_AGAIN_AT) != p.deliverAtOnce
        || !inData(queue)
        || !codeMatches(p.eventEntry, eventEntryOpening)
        || !codeMatches(p.deliverAtOnce, deliverAtOnceOpening)
        || displacementTarget(choice + DISPATCH_ENTRY_AT) != p.eventEntry
        || displacementTarget(choice + DISPATCH_QUEUE_AT) != queue
        || !codeMatches(displacementTarget(choice + DISPATCH_TEST_AT),
                        queueTestOpening))
        return partTwoNotFound(atOnce);

    const uint64_t update = functionNamed("Menu2::RootGame::PostRenderUpdate",
                                          rootUpdateSignature, &length);
    const uint64_t count = onlyPlaceIn(update, length, countdownPattern);
    const uint64_t call = onlyPlaceIn(update, length, skipToCallPattern);
    uint64_t skipToLength = 0;
    const uint64_t skipTo = functionNamed("GameTaskSubnode::SkipTo",
                                          skipToSignature, &skipToLength);
    unsigned counted = 0, idle = 0;
    if (!count || !call || !skipTo
        || !readDword(count + COUNTDOWN_READ_AT, &counted)
        || !readDword(count + COUNTDOWN_IDLE_AT, &idle)
        || counted != p.countdown || idle != p.countdown
        || displacementTarget(count + COUNTDOWN_GETTER_AT) != getter
        || displacementTarget(count + COUNTDOWN_TARGET_AT) != p.skipTarget
        || displacementTarget(call + SKIP_TO_CALL_AT) != skipTo
        || !codeMatches(count, COUNTDOWN_READ_BYTES))
        return partTwoNotFound("the skip countdown in "
                               "Menu2::RootGame::PostRenderUpdate");
    p.rootUpdate = update;
    p.countdownRead = count;

    const uint64_t hide = onlyPlaceIn(global_textBase, global_textSize,
                                      hideInterfacePattern);
    p.menuRoot = hide ? displacementTarget(hide + HIDE_MENU_ROOT_AT) : 0;
    if (!hide || displacementTarget(hide + HIDE_GETTER_AT) != getter
        || !readDword(hide + HIDE_IN_MENU_SLOT_AT, &p.inMenuSlot)
        || !p.inMenuSlot || (p.inMenuSlot & 7)
        || p.inMenuSlot >= PART_TWO_ROOT_SLOTS * 8
        || !codeMatches(p.menuRoot, menuRootOpening))
        return partTwoNotFound("the game's test for an in-game menu, and the "
                               "menu root (Menu2::RootGame)");

    if (!inData(p.managerPointer) || !inData(p.taskManager)
        || !inData(p.modeWord) || !inData(p.speedrunPointer)
        || !inData(p.scriptManager) || !p.countdown || (p.countdown & 3)
        || p.countdown >= PART_TWO_LARGEST_FIELD)
        return partTwoNotFound("the data the game's Skip Cinematic uses");
    global_partTwoParts = p;
    return 1;
}

/*
    The prompt's parts in Part II (PromptParts), each found by a pattern of
    code that stands once in the game's code, unless said otherwise:

      the interface root     the HUD getter, whose failure text is "Unable
                             to spawn HUD2::Root!": the handle, its id
                             beside it, and where the root holds its
                             process id
      get-or-create          its call that makes the game's own look-at
                             prompt (hud-look-at/look-root) under the
                             interface root, with the arguments the prompt
                             is made with; its search of a parent's
                             children, which stands CHILD_SEARCH_AT bytes
                             into it, gives the first child, the next
                             sibling and where a widget holds its widget
                             file's id
      assign category        the whole function, which adds categories to a
                             widget's own and names where it holds them
                             twice, the same
      the class tables       in the constructors of Gui2::TextWidget and
                             Gui2::ImageWidget
      IsVisible              Gui2::Widget's IsVisible: its slot is the one
                             of the Text table that holds it, and the Image
                             table holds it there too
      the property setter    its slot, from a float property set through
                             it; the Image table's setter there opens with
                             its compare of the id of "value"
      the laid-out rectangle an Image's nine-slice layout's read of the
                             laid-out width, eight bytes into the rectangle
      the key icon writer    the stack text builder's maker, which stands
                             more than once: the one whose call goes to a
                             function opening as the writer does, with the
                             builder's class
      title-cards            bit 22 (Part I's bit 21 is hud-menus here),
                             which the categories the game turns off for a
                             cinematic leave on
*/
static const char* const hudGetterPattern =
    "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 1B 8B 05 ?? ?? ?? ?? 85 C0 "
    "74 11 48 8B 51 08 48 85 D2 74 08 39 82 ?? 00 00 00 74 18 33 C9 E8 ?? ?? "
    "?? ?? 48 85 C0 75 0C 48 8D 0D";
#define HUD_HANDLE_AT            0x07   /* mov rcx, [the handle] */
#define HUD_ID_AT                0x12   /* mov eax, [the handle's id] */
#define HUD_PROCESS_ID_AT        0x25   /* cmp [rdx + the process id], eax */
#define HUD_FAILURE_AT           0x3A   /* lea rcx, [the failure text] */
static const char* const hudFailureText = "Unable to spawn HUD2::Root!\n";

/* The look-at prompt's get-or-create: the instance name's id (of
   ss-look-at-prompt), the arguments, the widget file's name, the call. */
static const char* const lookPromptPattern =
    "48 B8 CA 5A 9A 22 72 0C 6C 91 4C 89 44 24 28 48 8D 54 24 40 44 88 44 24 "
    "20 48 8B CF 4C 89 44 24 50 4C 89 44 24 48 4C 8D 05 ?? ?? ?? ?? 48 89 44 "
    "24 40 E8";
#define LOOK_FILE_AT             0x29   /* lea r8, [the widget file's name] */
#define LOOK_CALL_AT             0x33   /* call get-or-create */
static const char* const lookPromptFile = "hud-look-at/look-root";
static const char* const getOrCreateOpening =
    "40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 88 00 00 00";

/* Get-or-create's search: the first child, each child's widget file id and
   instance name compared, the next sibling. */
#define CHILD_SEARCH_AT          0x58   /* in get-or-create */
static const char* const childSearchPattern =
    "49 8B CF E8 ?? ?? ?? ?? 48 8B F0 48 85 C0 74 ?? 0F 1F 84 00 00 00 00 00 "
    "48 8B 06 48 8D 54 24 50 48 8B BE ?? ?? ?? ?? 48 8B CE FF 90 ?? ?? ?? ?? "
    "49 8B 06 48 39 44 24 50 75 ?? 49 8B 46 08 48 39 44 24 58 75 ?? 49 8B 46 "
    "10 48 39 44 24 60 75 ?? 48 85 DB 0F 84 ?? ?? ?? ?? 48 3B FB 0F 84 ?? ?? "
    "?? ?? 48 8B CE E8";
#define SEARCH_FIRST_CHILD_AT    0x04   /* call the first child */
#define SEARCH_FILE_ID_AT        0x23   /* mov rdi, [rsi + the widget file id] */
#define SEARCH_NEXT_SIBLING_AT   0x66   /* call the next sibling */

static const char* const assignCategoryPattern =
    "44 8B 81 ?? ?? 00 00 41 8B C0 0B C2 89 81 ?? ?? 00 00 44 3B C0 74 07 80 "
    "89 ?? ?? 00 00 20 C3";
#define ASSIGN_OWN_AT            0x03   /* mov r8d, [rcx + own categories] */
#define ASSIGN_OWN_AGAIN_AT      0x0E   /* mov [rcx + own categories], eax */

static const char* const textClassPattern =
    "C6 83 B0 05 00 00 01 48 8D 05 ?? ?? ?? ?? 48 89 03 48 8D 05 ?? ?? ?? ?? "
    "48 89 83 C0 04 00 00 80 A3 B8 05 00 00 E0";
#define TEXT_CLASS_AT            0x0A   /* lea rax, [the Text class table] */
static const char* const imageClassPattern =
    "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 03 "
    "48 8D 05 ?? ?? ?? ?? 48 89 83 C0 04 00 00 33 C0 48 89 83 C8 04 00 00 89 "
    "83 D0 04 00 00 48 89 83 D8 04 00 00";
#define IMAGE_CLASS_AT           0x11   /* lea rax, [the Image class table] */

static const char* const isVisiblePattern =
    "48 89 5C 24 08 57 48 83 EC 20 F6 81 ?? ?? 00 00 10 48 8B D9 75 ?? 33 D2 "
    "8D 4A 17 E8";

/* A float (BoxedValue type 6) set through the setter's slot. */
static const char* const setFloatPattern =
    "C7 44 24 2C 06 00 00 00 89 74 24 24 FF 90 ?? ?? 00 00";
#define SET_FLOAT_SLOT_AT        0x0E   /* call [rax + the setter's slot] */
static const char* const imageSetterOpening =
    "40 53 48 83 EC 20 48 B8 EA 0C E8 30 94 FD E4 7C";

static const char* const layoutWidthPattern =
    "C5 FA 10 9B ?? ?? 00 00 C5 E2 5C C0 C5 FA 59 D4 C5 E2 5C E6 C5 F2 5F EA "
    "C5 DA 59 C7 C5 FA 58 CE C5 F2 5F DE C5 DA 5C D5 C5 E2 5D C2 C5 FA 11 44 "
    "24 20";
#define LAYOUT_WIDTH_AT          0x04   /* vmovss xmm3, [rbx + the laid-out width] */

/* The builder's maker: its class, capacity 256, length 0, the text empty,
   then the call of a writer (the key's code, the builder, large). */
static const char* const iconMakerPattern =
    "48 8D 05 ?? ?? ?? ?? 48 89 44 24 20 48 C7 44 24 28 00 01 00 00 C6 44 24 "
    "38 00 48 C7 44 24 30 00 00 00 00 44 0F B6 C2 48 8D 54 24 20 E8";
#define MAKER_CLASS_AT           0x03   /* lea rax, [the builder's class] */
#define MAKER_WRITER_AT          0x2D   /* call the writer */
#define PROMPT_MAKERS            8      /* the most makers looked at */
static const char* const keyIconWriterOpening =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 40 48 8B FA "
    "45 0F B6 F0 48 8B D1 48 8D 4C 24 30 E8";

/* The categories the game turns off for a cinematic, a bit each. */
static const char* const cinematicMaskPattern =
    "BA D5 EF BF FF 41 B8 01 00 00 00 48 8B C8";
#define CINEMATIC_MASK_AT        0x01   /* mov edx, the categories turned off */
#define PART_TWO_TITLE_CARDS_BIT (1u << 22)

#define PROMPT_FIELD_LIMIT       0x1000 /* a field's place in an object is below this */
#define PROMPT_TABLE_SLOTS       128    /* class table slots searched for IsVisible */

static int promptNotFound(const char* what) {
    writeLog("PROMPT %s was not found in the game's code as the Scene Skipper "
             "expects it, so no prompt is shown; the skip key still skips",
             what);
    return 0;
}

/* Whether the game's data at address holds text and its zero byte. */
static int dataHoldsText(uint64_t address, const char* text) {
    const uint64_t length = strlen(text) + 1;
    if (!inData(address) || !inData(address + length - 1)) return 0;
    int same = 0;
    GUARD_TRY {
        same = memcmp((const void*)(uintptr_t)address, text,
                      (size_t)length) == 0;
    } GUARD_EXCEPT { same = 0; }
    return same;
}

/* A field's place read from the code: above 0, below PROMPT_FIELD_LIMIT and
   a multiple of size. */
static int fieldPlaceFits(unsigned place, unsigned size) {
    return place && place < PROMPT_FIELD_LIMIT && !(place % size);
}

/* Every part of Part II's prompt, found and checked into
   global_promptParts; 1 when all are, else 0 with one line in the log. */
static int findPartTwoPrompt(void) {
    PromptParts p;
    memset(&p, 0, sizeof(p));
    const uint64_t code = global_textBase, codeSize = global_textSize;

    const uint64_t getter = onlyPlaceIn(code, codeSize, hudGetterPattern);
    p.hudHandle = getter ? displacementTarget(getter + HUD_HANDLE_AT) : 0;
    if (!getter || !inData(p.hudHandle)
        || displacementTarget(getter + HUD_ID_AT) != p.hudHandle + HANDLE_ID
        || !readDword(getter + HUD_PROCESS_ID_AT, &p.hudProcessId)
        || !fieldPlaceFits(p.hudProcessId, 4)
        || !dataHoldsText(displacementTarget(getter + HUD_FAILURE_AT),
                          hudFailureText))
        return promptNotFound("the interface root (HUD2::Root)");

    const uint64_t look = onlyPlaceIn(code, codeSize, lookPromptPattern);
    p.getOrCreate = look ? displacementTarget(look + LOOK_CALL_AT) : 0;
    if (!look || !codeMatches(p.getOrCreate, getOrCreateOpening)
        || !dataHoldsText(displacementTarget(look + LOOK_FILE_AT),
                          lookPromptFile))
        return promptNotFound("the widget system's get-or-create");
    const uint64_t search = p.getOrCreate + CHILD_SEARCH_AT;
    p.firstChild = displacementTarget(search + SEARCH_FIRST_CHILD_AT);
    p.nextSibling = displacementTarget(search + SEARCH_NEXT_SIBLING_AT);
    if (!codeMatches(search, childSearchPattern) || !inCode(p.firstChild)
        || !inCode(p.nextSibling)
        || !readDword(search + SEARCH_FILE_ID_AT, &p.widgetFileId)
        || !fieldPlaceFits(p.widgetFileId, 8))
        return promptNotFound("the widget system's walk through a widget's "
                              "children");

    p.assignCategory = onlyPlaceIn(code, codeSize, assignCategoryPattern);
    unsigned ownAgain = 0;
    if (!p.assignCategory
        || !readDword(p.assignCategory + ASSIGN_OWN_AT, &p.ownCategories)
        || !readDword(p.assignCategory + ASSIGN_OWN_AGAIN_AT, &ownAgain)
        || ownAgain != p.ownCategories || !fieldPlaceFits(p.ownCategories, 4))
        return promptNotFound("the widget system's assignment of a widget's "
                              "categories");

    const uint64_t textMaker = onlyPlaceIn(code, codeSize, textClassPattern);
    const uint64_t imageMaker = onlyPlaceIn(code, codeSize, imageClassPattern);
    p.textClass = textMaker ? displacementTarget(textMaker + TEXT_CLASS_AT) : 0;
    p.imageClass = imageMaker ? displacementTarget(imageMaker + IMAGE_CLASS_AT)
                              : 0;
    if (!inData(p.textClass) || !inData(p.imageClass)
        || p.textClass == p.imageClass)
        return promptNotFound("the class tables of a Text and an Image");

    const uint64_t isVisible = onlyPlaceIn(code, codeSize, isVisiblePattern);
    int slots = 0;
    for (unsigned slot = 0; isVisible && slot < PROMPT_TABLE_SLOTS; ++slot) {
        uint64_t method = 0;
        if (!readQword(p.textClass + 8ull * slot, &method)) {
            slots = 0;
            break;
        }
        if (method == isVisible) {
            p.isVisibleSlot = 8u * slot;
            ++slots;
        }
    }
    uint64_t imageMethod = 0;
    if (slots != 1
        || !readQword(p.imageClass + p.isVisibleSlot, &imageMethod)
        || imageMethod != isVisible)
        return promptNotFound("a widget's IsVisible");

    const uint64_t setFloat = onlyPlaceIn(code, codeSize, setFloatPattern);
    uint64_t imageSetter = 0;
    if (!setFloat
        || !readDword(setFloat + SET_FLOAT_SLOT_AT, &p.setPropertySlot)
        || !p.setPropertySlot || (p.setPropertySlot & 7)
        || p.setPropertySlot >= PROMPT_TABLE_SLOTS * 8
        || !readQword(p.imageClass + p.setPropertySlot, &imageSetter)
        || !codeMatches(imageSetter, imageSetterOpening))
        return promptNotFound("a widget's property setter");

    const uint64_t layout = onlyPlaceIn(code, codeSize, layoutWidthPattern);
    unsigned width = 0;
    if (!layout || !readDword(layout + LAYOUT_WIDTH_AT, &width)
        || width <= 8 || !fieldPlaceFits(width, 4))
        return promptNotFound("a widget's laid-out rectangle");
    p.layoutRect = width - 8;

    uint64_t makers[PROMPT_MAKERS];
    const int makerCount = placesIn(code, codeSize, iconMakerPattern, makers,
                                    PROMPT_MAKERS);
    int writers = 0;
    for (int i = 0; i < makerCount && i < PROMPT_MAKERS; ++i) {
        const uint64_t writer = displacementTarget(makers[i] + MAKER_WRITER_AT);
        if (!codeMatches(writer, keyIconWriterOpening)) continue;
        ++writers;
        p.keyIconText = writer;
        p.builderClass = displacementTarget(makers[i] + MAKER_CLASS_AT);
    }
    if (makerCount > PROMPT_MAKERS || writers != 1 || !inData(p.builderClass))
        return promptNotFound("the game's key icon writer");

    const uint64_t hide = onlyPlaceIn(code, codeSize, cinematicMaskPattern);
    unsigned turnedOff = 0;
    p.titleCards = PART_TWO_TITLE_CARDS_BIT;
    if (!hide || !readDword(hide + CINEMATIC_MASK_AT, &turnedOff)
        || (turnedOff & p.titleCards))
        return promptNotFound("the interface categories a cinematic turns "
                              "off");

    global_promptParts = p;
    global_promptPartsFound = 1;
    return 1;
}

/* The window in front belongs to the game's process. */
static int gameWindowInFront(void) {
    HWND front = GetForegroundWindow();
    if (!front) return 0;
    DWORD process = 0;
    GetWindowThreadProcessId(front, &process);
    return process == GetCurrentProcessId();
}

/* The skip key held down with the game window in front. */
static int partTwoKeyDown(int key) {
    if (key <= 0 || key > 0xFF || !gameWindowInFront()) return 0;
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

static void partTwoSayOnce(int which, const char* line) {
    if (global_partTwoSaid & which) return;
    global_partTwoSaid |= which;
    writeLog("%s", line);
}

/* The menu root, where its class table holds the PostRenderUpdate whose
   countdown was found; 0 otherwise. */
static uint64_t partTwoMenuRoot(void) {
    uint64_t root = 0, table = 0;
    GUARD_TRY { root = ((MenuRootFn)global_partTwoParts.menuRoot)(); }
    GUARD_EXCEPT { root = 0; }
    if (!root || !readQword(root, &table) || !inModule(table)) return 0;
    if (table == global_partTwoRootTable) return root;
    for (unsigned slot = 0; slot < PART_TWO_ROOT_SLOTS; ++slot) {
        uint64_t method = 0;
        if (!readQword(table + 8ull * slot, &method)) return 0;
        if (method == global_partTwoParts.rootUpdate) {
            global_partTwoRootTable = table;
            return root;
        }
    }
    return 0;
}

/* 1 while an in-game menu is up, as the game asks before it hides the
   interface for a cinematic; 0 while none is; -1 when it cannot be asked. */
static int partTwoMenuUp(uint64_t root) {
    const uint64_t method = methodOf(root, global_partTwoParts.inMenuSlot);
    if (!method) return -1;
    int up = -1;
    GUARD_TRY { up = ((InMenuFn)method)(root, 0) != 0; }
    GUARD_EXCEPT { up = -1; }
    return up;
}

static const char* const offerFaulted =
    "SKIP asking the game for its Skip Cinematic faulted, so the skip key made "
    "no skip";

/* 1 while the pause menu would show its Skip Cinematic row, as its own test
   asks: the row's byte clear and SkipOffered(manager) true; 0 while it would
   not; -1 when the game cannot be asked. */
static int partTwoRowShown(uint64_t manager) {
    unsigned char hidden = 0;
    if (!readByte(global_partTwoParts.rowHidden, &hidden)) return -1;
    if (hidden) return 0;
    int offered = -1;
    GUARD_TRY {
        offered = ((SkipOfferedFn)global_partTwoParts.skipOffered)(manager) != 0;
    } GUARD_EXCEPT { offered = -1; }
    return offered;
}

/*
    Whether the key offers a skip now, which the prompt shows: steps 1 to 3
    of the Yes branch. 1, with SkipTarget's target, skip to and checkpoint,
    while the pause menu would show its Skip Cinematic row, SkipTarget gives
    a target and the target ends neither an encounter or cinematic selection
    nor a speedrun; 0 otherwise; -1 when the game cannot be asked, logged
    once. The menus and the countdown are not asked.
*/
static int partTwoOffer(uint64_t* node, uint64_t* skipTo, uint64_t* checkpoint) {
    const PartTwoParts* p = &global_partTwoParts;
    *node = *skipTo = *checkpoint = 0;
    uint64_t manager = 0;
    if (!readQword(p->managerPointer, &manager) || !manager) return 0;
    const int offered = partTwoRowShown(manager);
    if (offered < 0) partTwoSayOnce(SAID_OFFER, offerFaulted);
    if (offered <= 0) return offered;

    uint64_t target = 0;
    int asked = 1;
    GUARD_TRY {
        target = ((SkipTargetFn)p->skipTarget)(manager, skipTo, checkpoint);
    } GUARD_EXCEPT { asked = 0; }
    if (!asked) {
        partTwoSayOnce(SAID_OFFER, offerFaulted);
        return -1;
    }
    if (!target) return 0;

    int elsewhere = 0;
    GUARD_TRY {
        unsigned mode = 0;
        if (readDword(p->modeWord, &mode)
            && (mode == MODE_ENCOUNTER_SELECTION
                || mode == MODE_CINEMATIC_SELECTION)) {
            const uint64_t graph = ((FirstGraphFn)p->firstGraph)(p->taskManager);
            elsewhere = !graph
                     || ((SelectionEndsFn)p->selectionEnds)(graph, target,
                                                            *checkpoint) != 0;
        }
        uint64_t speedrun = 0;
        if (!elsewhere && readQword(p->speedrunPointer, &speedrun))
            elsewhere = ((SpeedrunOnFn)p->speedrunOn)(speedrun) != 0
                     && ((SpeedrunEndsFn)p->speedrunEnds)(speedrun, target) != 0;
    } GUARD_EXCEPT { asked = 0; }
    if (!asked) {
        partTwoSayOnce(SAID_TESTS, "SKIP the game's test for the end of a "
                                   "selection or a speedrun faulted, so the "
                                   "skip key made no skip");
        return -1;
    }
    if (elsewhere) return 0;
    *node = target;
    return 1;
}

/* A press of the key, while partTwoOffer offers a skip, the menu root is
   the one whose update makes the skip, no in-game menu is up and no skip
   counts down: the countdown set to 2 and the skip left to the read of the
   countdown that finds 0 (partTwoCountdownRead). Nothing is broadcast
   here. */
static void partTwoSkip(void) {
    static const char* const countdownLost =
        "SKIP the menu root's skip countdown could not be read or written, so "
        "the skip key made no skip";
    const PartTwoParts* p = &global_partTwoParts;
    uint64_t node = 0, skipTo = 0, checkpoint = 0;
    if (partTwoOffer(&node, &skipTo, &checkpoint) <= 0) return;

    const uint64_t root = partTwoMenuRoot();
    if (!root) {
        partTwoSayOnce(SAID_ROOT, "SKIP the menu root (Menu2::RootGame) was "
                                  "not there, or not the one whose update "
                                  "makes the skip, so the skip key made no "
                                  "skip");
        return;
    }
    const int menuUp = partTwoMenuUp(root);
    if (menuUp < 0)
        partTwoSayOnce(SAID_MENU, "SKIP the game could not be asked whether an "
                                  "in-game menu is up, so the skip key made no "
                                  "skip");
    if (menuUp) return;
    unsigned waiting = 0;
    if (!readDword(root + p->countdown, &waiting)) {
        partTwoSayOnce(SAID_COUNTDOWN, countdownLost);
        return;
    }
    if ((int)waiting >= 0) return;

    global_partTwoWaitNode = node;
    global_partTwoWaitSkipTo = skipTo;
    global_partTwoWaitCheckpoint = checkpoint;
    global_partTwoWaitTicks = 0;
    if (!writeDword(root + p->countdown, PART_TWO_COUNTDOWN)) {
        partTwoSayOnce(SAID_COUNTDOWN, countdownLost);
        return;
    }
    InterlockedExchange(&global_partTwoWaitAbandoned, 0);
    InterlockedExchange64(&global_partTwoWaitRoot, (LONG64)root);
}

/* Whether the game still offers the skip the key started: the row shown and
   SkipTarget answering the target, skip to and checkpoint it answered at the
   press. 0 too when the game cannot be asked, logged once. */
static int partTwoStillOffered(void) {
    const PartTwoParts* p = &global_partTwoParts;
    uint64_t manager = 0;
    if (!readQword(p->managerPointer, &manager) || !manager) return 0;
    const int offered = partTwoRowShown(manager);
    uint64_t node = 0, skipTo = 0, checkpoint = 0;
    int asked = offered >= 0;
    if (offered > 0) {
        GUARD_TRY {
            node = ((SkipTargetFn)p->skipTarget)(manager, &skipTo, &checkpoint);
        } GUARD_EXCEPT { asked = 0; }
    }
    if (!asked) partTwoSayOnce(SAID_OFFER, offerFaulted);
    return asked && offered > 0 && node == global_partTwoWaitNode
        && skipTo == global_partTwoWaitSkipTo
        && checkpoint == global_partTwoWaitCheckpoint;
}

/* GameInfo's table entry PART_TWO_EVENT_ENTRY as the getter answers it,
   where the bytes of it the setter reads and writes are writable memory; 0
   where the getter faults or answers 0 or other memory. */
static uint64_t partTwoEventEntry(void) {
    uint64_t entry = 0;
    GUARD_TRY {
        entry = ((EventEntryFn)global_partTwoParts.eventEntry)(
            PART_TWO_EVENT_ENTRY);
    } GUARD_EXCEPT { entry = 0; }
    if (!entry || entry > LARGEST_POINTER - eventEntryTouchedEnd
        || !writableMemory(entry + eventEntryTouchedFrom,
                           eventEntryTouchedEnd - eventEntryTouchedFrom))
        return 0;
    return entry;
}

/* Step 4's broadcast: cinematic-skip through the state script manager, with
   the skip to and the checkpoint SkipTarget answered at the press as names
   and two empty arguments, delivered at once in the order of the game's
   drain of the event queue: the entry from the getter, its deliver-at-once
   byte set to 1 through the game's setter, the broadcast, then the entry
   from the getter again and the byte given back its old value through the
   setter. The setter takes and releases the entry's lock, and the broadcast
   runs the state scripts' handlers, which hold the state script manager;
   neither call may be guarded, since a fault swallowed inside either can
   leave the lock or the hold in place. 0, with nothing broadcast, where the
   entry cannot be had, logged once. */
static int partTwoBroadcast(void) {
    const PartTwoParts* p = &global_partTwoParts;
    BoxedValue skipToName = { global_partTwoWaitSkipTo, 0, BOXED_NAME };
    BoxedValue atCheckpoint = { global_partTwoWaitCheckpoint, 0, BOXED_NAME };
    BoxedValue none = { 0, 0, 0 };
    BoxedValue alsoNone = { 0, 0, 0 };
    const uint64_t event = idOfName("cinematic-skip");
    const uint64_t entry = partTwoEventEntry();
    if (!entry) {
        partTwoSayOnce(SAID_BROADCAST, "SKIP the game could not be set to hand "
                                       "the skip event to the scripts at "
                                       "once, so the skip key made no skip");
        return 0;
    }
    const unsigned char was = ((DeliverAtOnceFn)p->deliverAtOnce)(entry, 1);
    ((BroadcastFn)p->broadcast)(p->scriptManager, event, &skipToName,
                                &atCheckpoint, &none, &alsoNone);
    const uint64_t again = partTwoEventEntry();
    if (again)
        ((DeliverAtOnceFn)p->deliverAtOnce)(again, was);
    else
        partTwoSayOnce(SAID_RESTORE, "SKIP the game's handing of events to the "
                                     "scripts could not be set back after the "
                                     "skip key's skip event");
    return 1;
}

/* The hook where PostRenderUpdate reads the countdown, r14 the root it
   updates, on every call. In the root the key set the countdown in: at 1 or
   2 the count goes on; at 0 the key's skip is decided, and the wait ends:
   still offered (and the wait not abandoned), cinematic-skip is broadcast,
   delivered at once, and the countdown left at 0, so the game's SkipTo
   follows in this call; else the countdown is set to -1 and nothing is
   broadcast. At any other value the wait ends with nothing done. */
static void partTwoCountdownRead(void* registers, void* user) {
    (void)user;
    const uint64_t root = loaderHookRegister(registers, LOADER_REG_R14);
    if (!root || root != (uint64_t)global_partTwoWaitRoot) return;
    const uint64_t countdown = root + global_partTwoParts.countdown;
    unsigned now = 0;
    if (!readDword(countdown, &now) || now == 1 || now == 2) return;
    if ((uint64_t)InterlockedCompareExchange64(&global_partTwoWaitRoot, 0,
                                               (LONG64)root) != root
        || now != 0)
        return;
    if (global_partTwoWaitAbandoned || !partTwoStillOffered()
        || !partTwoBroadcast()) {
        writeDword(countdown, PART_TWO_NO_COUNTDOWN);
        return;
    }
    uint64_t name = 0;
    readQword(global_partTwoWaitNode + NODE_NAME, &name);
    ++global_skipsMade;
    global_lastHow = SKIP_BY_GAME;
    global_lastScene = 0;
    global_lastTarget = name;
    global_lastFrame = global_lastFrom = 0.0f;
    global_lastFromPause = 0;
    global_lastTimeAdded = 0.0f;
}

/* Every tick while the key's skip waits and is not abandoned: once the menu
   root's getter no longer gives the root it was set in, or its countdown
   cannot be read, the wait is abandoned, with no write; once the countdown
   is no longer 0 to 2 the wait ends, with no write; and where the read of
   the countdown has not decided the skip within PART_TWO_WAIT_TICKS ticks,
   the countdown is set back to -1, logged once. */
static void partTwoWatchCountdown(void) {
    const uint64_t root = (uint64_t)global_partTwoWaitRoot;
    if (!root || global_partTwoWaitAbandoned) return;
    unsigned waiting = 0;
    if (partTwoMenuRoot() != root
        || !readDword(root + global_partTwoParts.countdown, &waiting)) {
        InterlockedExchange(&global_partTwoWaitAbandoned, 1);
        return;
    }
    if (waiting > PART_TWO_COUNTDOWN) {
        InterlockedCompareExchange64(&global_partTwoWaitRoot, 0, (LONG64)root);
        return;
    }
    if (++global_partTwoWaitTicks < PART_TWO_WAIT_TICKS) return;
    if ((uint64_t)InterlockedCompareExchange64(&global_partTwoWaitRoot, 0,
                                               (LONG64)root) != root)
        return;
    writeDword(root + global_partTwoParts.countdown, PART_TWO_NO_COUNTDOWN);
    partTwoSayOnce(SAID_NOT_COUNTED, "SKIP the game did not count down the "
                                     "skip the skip key started, so the skip "
                                     "key withdrew it");
}

/* Every tick in Part II: the countdown watched, and the key. */
static void partTwoSkipWhenAsked(void) {
    partTwoWatchCountdown();
    const int down = partTwoKeyDown(global_set.key);
    const int pressed = down && !global_skipKeyWasDown;
    global_skipKeyWasDown = down;
    if (pressed && keyOn()) partTwoSkip();
}

/* ------------------------------------------------------------ the tick */

static volatile LONG global_ticking = 0;

/* Every frame, from the game's script update, on the game's own thread. */
static void sceneSkipperTick(void* registers, void* user) {
    (void)registers; (void)user;
    if (InterlockedCompareExchange(&global_ticking, 1, 0) != 0) return;
    Settings fresh;
    if (settingsChanged(&fresh)) applySettings(&fresh);
    if (global_partTwo) {
        if (global_set.enabled) {
            partTwoSkipWhenAsked();
            ensureSkipPrompt();
            fadeSkipPrompt();
        }
    } else if (global_gameKnown && global_set.enabled) {
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
    with 0, so a toolkit built against another layout reads nothing wrong. In
    The Last of Us Part II: offered, target, label and phase as in Part I, a
    skip offered while partTwoOffer offers one; skips counts each time the
    read of the countdown broadcast cinematic-skip and left the countdown at 0
    for the game's SkipTo, whether or not SkipTo then plays the target, and
    lastHow and lastTarget are the last one's; the rest is 0.
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
    if (global_partTwo && global_set.enabled) {
        uint64_t node = 0, skipTo = 0, checkpoint = 0;
        view->offered = partTwoOffer(&node, &skipTo, &checkpoint) > 0 ? 1u : 0u;
        if (view->offered) {
            readQword(node + NODE_NAME, &view->target);
            if (promptOn())
                strncpy_s(view->label, sizeof(view->label), labelFor(0, 1),
                          _TRUNCATE);
        }
    } else if (global_gameKnown && global_set.enabled) {
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
extern "C" TLOU_PLUGIN_EXPORT const char* PluginVersion(void) { return "v1.1.0"; }
extern "C" TLOU_PLUGIN_EXPORT const char* PluginAuthor(void)  { return "mwith77"; }

extern "C" TLOU_PLUGIN_EXPORT int SupportsGame(LoaderHandle) { return 1; }

static volatile LONG global_initCalled = 0;

static DWORD WINAPI standalone(LPVOID) {
    Sleep(2000);
    if (!global_initCalled)
        writeLog("no loader present; nothing can be hooked");
    return 0;
}

static void pathsFrom(const char* modFolder) {
    formatInto(global_iniPath, sizeof(global_iniPath), "%s\\%s.ini", modFolder,
               modBaseName);
    formatInto(global_logPath, sizeof(global_logPath), "%s\\%s.log",
               modFolder, modBaseName);
}

extern "C" TLOU_PLUGIN_EXPORT int Init(LoaderHandle loader) {
    InterlockedExchange(&global_initCalled, 1);
    if (!loaderBind(loader)) return 0;
    /* Before anything else: a bounded call handed too much is reported
       through the loader instead of ending the process. */
    loaderWatchBadArguments(PluginName());

    const char* folder = loaderModFolder();
    if (folder && *folder) pathsFrom(folder);

    /* The default settings are put into the ini, then its write time is
       taken, then the settings are read. */
    writeDefaultSettings((HMODULE)&__ImageBase, global_iniPath);
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
    const uint32_t stamp = loaderGameTimeDateStamp();
    if (!stamp) {
        writeLog("the loader does not give the game's time stamp, so the "
                 "game's build cannot be checked and the Scene Skipper does "
                 "nothing");
        return 1;
    }
    global_gameKnown = stamp == GAME_BUILD_STAMP;
    if (!global_gameKnown && stamp != PART_TWO_STAMP
        && stamp != PART_TWO_L_STAMP) {
        const char* exe = loaderGameExeName();
        char game[96], built[48];
        if (!exe || !exe[0] || !formatInto(game, sizeof(game), "the game, %s,",
                                           exe))
            formatInto(game, sizeof(game), "the game");
        writeStampDate(built, sizeof(built), stamp);
        writeLog("%s is not tlou-i.exe 1.1.5.0, tlou-ii.exe 1.6.0 or "
                 "tlou-ii-l.exe 1.6.0, the builds the Scene Skipper is made "
                 "for (this game was built on %s), so the Scene Skipper does "
                 "nothing", game, built);
        return 1;
    }
    if (!global_gameKnown) {
        global_partTwo = findPartTwo();
        if (!global_partTwo) return 1;
        if (!loaderHookAddress("sceneskipper_skip_countdown",
                               global_partTwoParts.countdownRead,
                               partTwoCountdownRead, nullptr)) {
            global_partTwo = 0;
            writeLog("SKIP the loader refused the hook where "
                     "Menu2::RootGame::PostRenderUpdate reads the skip "
                     "countdown, so the skip key makes no skips");
            return 1;
        }
        if (findPartTwoPrompt() && !installPromptFiles())
            global_promptPartsFound = 0;
        if (!loaderHookFunction("ScriptManager::ListenerUpdate",
                                listenerUpdateSignature, sceneSkipperTick,
                                nullptr)) {
            global_partTwo = 0;
            writeLog("the loader refused the hook on "
                     "ScriptManager::ListenerUpdate, so the Scene Skipper does "
                     "nothing");
        }
        return 1;
    }

    const uint64_t found = loaderFindFunction("GameTaskSubnode::SkipTo",
                                              skipToSignature);
    GUARD_TRY { global_skipToFunction = found ? entryOfFunction(found) : 0; }
    GUARD_EXCEPT { global_skipToFunction = 0; }
    if (!global_skipToFunction)
        writeLog("GameTaskSubnode::SkipTo was not found, so the skip key makes "
                 "no skips to a task");

    fillPartOnePrompt();
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
    char folder[MAX_PATH];
    const DWORD length = GetModuleFileNameA(self, folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return TRUE;
    char* slash = strrchr(folder, '\\');
    if (slash) *slash = 0;
    pathsFrom(folder);

    CloseHandle(CreateThread(nullptr, 0, standalone, nullptr, 0, nullptr));
    return TRUE;
}
