/* Egg of Time's Steam helper (egg-steam.exe, 32-bit Windows): with a Steam version of the game connected, Egg of Time
 * (runtime/steam_presence.c) starts it so Steam shows the player in that game while Egg of Time runs. It loads the
 * steam_api.dll of the player's own Steam copy (a 32-bit DLL, which the 64-bit Egg of Time cannot load) and starts
 * the Steam API as the copy's app (its steam_appid.txt), then keeps it running until Egg of Time closes its end of
 * the pipe on standard input (or exits: its job object ends this process with it).
 *
 * Usage: egg-steam.exe <the Steam copy's folder>
 * Writes one line on standard output: "ok <app id>", or "error <reason>". */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int(__cdecl *InitFn)(void); /* bool */
typedef void(__cdecl *VoidFn)(void);

static HANDLE out;

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

/* Standard input reached its end: Egg of Time closed the pipe or is gone. */
static DWORD WINAPI watch_input(LPVOID done) {
    char buf[256];
    DWORD n;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    while (ReadFile(in, buf, sizeof buf, &n, NULL) && n) {}
    SetEvent((HANDLE)done);
    return 0;
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
    HANDLE done = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, watch_input, done, 0, NULL);
    if (t) CloseHandle(t);
    while (WaitForSingleObject(done, 100) == WAIT_TIMEOUT) run();
    shutdown();
    return 0;
}
