/* Steam: with a Steam version of the game connected (steam_edges.c), Steam shows the player in that game while Egg
 * of Time runs. The Steam copy's steam_api.dll is 32-bit, so a 32-bit helper next to the executable
 * (egg-steam.exe, tools/steam_helper) loads it and starts the Steam API as the copy's app; Steam then counts the
 * time as played in it. The helper is in a job object that ends it with Egg of Time, and it stops on its own when
 * the pipe on its standard input closes. Its one line of answer goes to the log. Windows only: elsewhere the Steam
 * copy is the Windows build, whose DLL a native Egg of Time cannot use. */
#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> /* before rt.h, whose macros name Windows' fields */
#endif

#include "rt.h"
#include "steam_edges.h"
#include "steam_presence.h"

#ifdef _WIN32
static int started;

static wchar_t *wide(const char *utf8) { return (wchar_t *)SDL_iconv_string("UTF-16LE", "UTF-8", utf8, SDL_strlen(utf8) + 1); }

/* The helper's answer, read on a thread of its own (it waits on Steam). */
static int SDLCALL read_answer(void *data) {
    HANDLE out = data;
    char line[256];
    DWORD n;
    /* steam_api.dll writes lines of its own there too ("Setting breakpad minidump AppID = ..."): the helper's
     * starts with "ok " or "error " */
    for (int lines = 0; lines < 32; lines++) {
        DWORD len = 0;
        int more = 0;
        while (len < sizeof line - 1 && (more = ReadFile(out, line + len, 1, &n, NULL) && n) && line[len] != '\n') len++;
        line[len] = 0;
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (!SDL_strncmp(line, "ok ", 3) || !SDL_strncmp(line, "error ", 6)) {
            rt_log("Steam: %s", line);
            CloseHandle(out);
            return 0;
        }
        if (!more) break;
    }
    rt_log("Steam: the helper gave no answer");
    CloseHandle(out);
    return 0;
}

void sp_start(void) {
    const char *dir = se_dir();
    if (started || !dir) return;
    started = 1;
    char exe[1100];
    SDL_snprintf(exe, sizeof exe, "%segg-steam.exe", SDL_GetBasePath());
    char cmd[2400];
    SDL_snprintf(cmd, sizeof cmd, "\"%s\" \"%s\"", exe, dir);
    wchar_t *wexe = wide(exe), *wcmd = wide(cmd);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit = {0};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit);
    if (wexe && wcmd && CreatePipe(&in_r, &in_w, &sa, 0) && CreatePipe(&out_r, &out_w, &sa, 0)) {
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si;
        SDL_zero(si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_r, si.hStdOutput = out_w, si.hStdError = out_w;
        PROCESS_INFORMATION pi;
        if (CreateProcessW(wexe, wcmd, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            if (job) AssignProcessToJobObject(job, pi.hProcess); /* the job handle stays open: closed when Egg of Time exits */
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
            in_w = NULL; /* the helper's standard input: left open while Egg of Time runs */
            SDL_Thread *t = SDL_CreateThread(read_answer, "steam", out_r);
            if (t) SDL_DetachThread(t), out_r = NULL;
        } else {
            rt_log("Steam: cannot start %s (error %lu)", exe, (unsigned long)GetLastError());
        }
    }
    if (in_r) CloseHandle(in_r);
    if (out_w) CloseHandle(out_w);
    if (in_w) CloseHandle(in_w);
    if (out_r) CloseHandle(out_r);
    SDL_free(wexe), SDL_free(wcmd);
}

#else
void sp_start(void) {}
#endif
