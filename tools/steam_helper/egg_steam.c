/* Egg of Time's Steam helper (egg-steam.exe, 32-bit Windows): with a Steam version of the game connected, Egg of Time
 * (runtime/steam_presence.c) starts it so Steam shows the player in that game while Egg of Time runs. It loads the
 * steam_api.dll of the player's own Steam copy (a 32-bit DLL, which the 64-bit Egg of Time cannot load) and starts
 * the Steam API as the copy's app (its steam_appid.txt), then keeps it running until Egg of Time closes its end of
 * the pipe on standard input (or exits: its job object ends this process with it).
 *
 * Usage: egg-steam.exe <the Steam copy's folder>
 * Writes a line on standard output first: "ok <app id>", or "error <reason>".
 * Then reads commands from standard input, one per line:
 *   unlock <achievement api name>   unlocks it on the player's account; answers "unlocked <name>" once Steam has
 *                                   it (an achievement already unlocked stays as it is).
 * Steam takes achievements only once the player's stats have arrived (RequestCurrentStats): until then the unlocks
 * wait, and are tried again every 100 ms. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int(__cdecl *InitFn)(void); /* bool */
typedef void(__cdecl *VoidFn)(void);
typedef void *(__cdecl *StatsFn)(void);                                /* ISteamUserStats* */
typedef int(__cdecl *StatsCallFn)(void *);                             /* bool RequestCurrentStats / StoreStats */
typedef int(__cdecl *NameCallFn)(void *, const char *);                /* bool SetAchievement */
typedef int(__cdecl *GetAchFn)(void *, const char *, unsigned char *); /* bool GetAchievement */

#define MAX_PENDING 64

static HANDLE out;
static CRITICAL_SECTION lock;
static char pending[MAX_PENDING][128]; /* achievements to unlock (under lock) */
static int pending_count;

static void say(const char *line) {
    DWORD n;
    WriteFile(out, line, (DWORD)strlen(line), &n, NULL);
    WriteFile(out, "\n", 1, &n, NULL);
}

/* The app id in <dir>/steam_appid.txt, or 0. */
static unsigned long app_id(const wchar_t *dir) {
    wchar_t path[MAX_PATH * 2];
    _snwprintf(path, MAX_PATH * 2, L"%ls\\steam_appid.txt", dir);
    FILE *f = _wfopen(path, L"r");
    unsigned long id = 0;
    if (f) {
        if (fscanf(f, "%lu", &id) != 1) id = 0;
        fclose(f);
    }
    return id;
}

static void command(const char *line) {
    if (!strncmp(line, "unlock ", 7) && line[7] && strlen(line + 7) < sizeof pending[0]) {
        EnterCriticalSection(&lock);
        if (pending_count < MAX_PENDING) strcpy(pending[pending_count++], line + 7);
        LeaveCriticalSection(&lock);
    }
}

/* Reads the commands; sets done once standard input reaches its end (Egg of Time closed the pipe or is gone). */
static DWORD WINAPI watch_input(LPVOID done) {
    char buf[256], line[256];
    size_t len = 0;
    DWORD n;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    while (ReadFile(in, buf, sizeof buf, &n, NULL) && n) {
        for (DWORD i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (len && line[len - 1] == '\r') len--;
                line[len] = 0;
                command(line);
                len = 0;
            } else if (len < sizeof line - 1) {
                line[len++] = buf[i];
            }
        }
    }
    SetEvent((HANDLE)done);
    return 0;
}

/* The flat Steam API's user stats calls (NULL where this steam_api.dll lacks them). */
static struct {
    void *stats;
    StatsCallFn request, store;
    NameCallFn set;
    GetAchFn get;
    int requested;
} user;

static void stats_init(HMODULE api) {
    StatsFn get = (StatsFn)(void *)GetProcAddress(api, "SteamAPI_SteamUserStats_v012");
    user.request = (StatsCallFn)(void *)GetProcAddress(api, "SteamAPI_ISteamUserStats_RequestCurrentStats");
    user.store = (StatsCallFn)(void *)GetProcAddress(api, "SteamAPI_ISteamUserStats_StoreStats");
    user.set = (NameCallFn)(void *)GetProcAddress(api, "SteamAPI_ISteamUserStats_SetAchievement");
    user.get = (GetAchFn)(void *)GetProcAddress(api, "SteamAPI_ISteamUserStats_GetAchievement");
    if (get && user.request && user.store && user.set && user.get) user.stats = get();
}

/* Unlocks what is pending, once Steam has the player's stats: GetAchievement fails until then. */
static void unlock_pending(void) {
    if (!user.stats) return;
    if (!user.requested) user.requested = user.request(user.stats) & 0xFF;
    char line[160];
    EnterCriticalSection(&lock);
    int kept = 0, changed = 0;
    for (int i = 0; i < pending_count; i++) {
        unsigned char done = 0;
        if (!(user.get(user.stats, pending[i], &done) & 0xFF)) {
            strcpy(pending[kept++], pending[i]); /* stats not in yet (or an unknown name: retried, harmless) */
            continue;
        }
        if (!done && (user.set(user.stats, pending[i]) & 0xFF)) changed = 1;
        snprintf(line, sizeof line, "unlocked %s", pending[i]);
        say(line);
    }
    pending_count = kept;
    LeaveCriticalSection(&lock);
    if (changed) user.store(user.stats); /* sends them to Steam, which shows its own notification */
}

int main(void) {
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc); /* the folder may not be in the ANSI code page */
    if (!argv || argc < 2) {
        say("error usage: egg-steam.exe <Steam copy folder>");
        return 2;
    }
    const wchar_t *dir = argv[1];
    unsigned long id = app_id(dir);
    if (!id) {
        say("error no steam_appid.txt in the Steam copy");
        return 1;
    }
    /* The Steam API takes the app from these (as when Steam starts the game) */
    char text[32];
    snprintf(text, sizeof text, "%lu", id);
    SetEnvironmentVariableA("SteamAppId", text);
    SetEnvironmentVariableA("SteamGameId", text);
    SetDllDirectoryW(dir);
    wchar_t dll[MAX_PATH * 2];
    _snwprintf(dll, MAX_PATH * 2, L"%ls\\steam_api.dll", dir);
    HMODULE api = LoadLibraryW(dll);
    if (!api) {
        say("error cannot load the Steam copy's steam_api.dll");
        return 1;
    }
    InitFn init = (InitFn)(void *)GetProcAddress(api, "SteamAPI_Init");
    VoidFn run = (VoidFn)(void *)GetProcAddress(api, "SteamAPI_RunCallbacks");
    VoidFn shutdown = (VoidFn)(void *)GetProcAddress(api, "SteamAPI_Shutdown");
    if (!init || !run || !shutdown) {
        say("error steam_api.dll lacks SteamAPI_Init");
        return 1;
    }
    if (!(init() & 0xFF)) {
        say("error SteamAPI_Init failed (Steam not running, or the game not owned by this account)");
        return 1;
    }
    snprintf(text, sizeof text, "ok %lu", id);
    say(text);
    stats_init(api);
    InitializeCriticalSection(&lock);
    HANDLE done = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, watch_input, done, 0, NULL);
    if (t) CloseHandle(t);
    while (WaitForSingleObject(done, 100) == WAIT_TIMEOUT) {
        run();
        unlock_pending();
    }
    shutdown();
    return 0;
}
