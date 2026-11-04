/* RetroAchievements: rcheevos' rc_client checks the achievements of the game's set against main RAM and DTCM once per
 * game frame (on the game thread, at VBlank, so memory is read between frames as an emulator would), and libcurl
 * carries its server calls, each on a thread of its own. Softcore only: save states, fast forward and mods stay
 * usable, and unlocks count as softcore.
 *
 * The login lives in retroachievements.ini in the data folder: the player's name and the login token the server
 * returned, never the password. The System Settings menu logs in with a name and password; a player without a
 * keyboard can instead write "username = <name>" and "password = <password>" into the file, and the next start logs
 * in and replaces the password with the token. A token the server refuses (expired, or the password changed) is
 * dropped from the file.
 *
 * The game is identified by rcheevos' hash of the ROM file (header, ARM9 and ARM7 code, icon), so a ROM the set does
 * not list (another region, a hack) gets no achievements. Unlocks, the game's progress and server problems show in
 * the window's message box, one after another; unlocks show as popups in the window's top right corner, each with the
 * achievement's badge (waiting up to POPUP_WAIT_MS for it to download), title and points.
 *
 * Achievement badges (System Settings' list) are downloaded when first shown, a few at a time, each on a thread of
 * its own, and kept as the server's PNG files in the data folder's cache/badges, so later starts read them there. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <curl/curl.h>
#include <rc_client.h>
#include <rc_consoles.h>

#include "cheevos.h"
#include "display.h"
#include "rt.h"

#ifndef EGG_VERSION
#define EGG_VERSION "dev"
#endif

#define NOTIFY_MS 4000
#define POPUP_MS 5000
#define POPUP_WAIT_MS 3000 /* for the badge, before showing the popup without it */
#define QUEUE_LEN 8
#define HTTP_TIMEOUT_S 30
#define LOGIN_LEN 256
#define DTCM_RA_ADDR 0x1000000u /* rcheevos' DS map: main RAM at 0, DTCM here */
#define LIST_REFRESH_MS 500
#define BADGE_FETCHES 4 /* downloads at once */
/* rcheevos' own warnings (e.g. "Unknown Emulator") come as achievements from this ID on (RC_CLIENT_ACHIEVEMENT_WARNING_ID,
 * private to rc_client.c); the list leaves them out, as rcheevos' summary does. */
#define WARNING_ID 101000001u

static rc_client_t *client;
static char user_agent[256];
static char ini_path[1024];
static SDL_AtomicInt state; /* CHEEVOS_* */
static SDL_Mutex *lock;     /* user, queue */
static char user[64];       /* logged in, or logging in */
static char queue[QUEUE_LEN][64];
static int queue_head, queue_count;
static Uint64 shown_until;
typedef struct Popup { char title[64], url[192]; unsigned points; Uint64 queued; } Popup;
static Popup popups[QUEUE_LEN]; /* lock */
static int popup_head, popup_count;
static Uint64 popup_until;
static char badge_dir[1024];

static void notify(const char *fmt, ...) {
    char text[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    rt_log("cheevos: %s", text);
    SDL_LockMutex(lock);
    if (queue_count < QUEUE_LEN) memcpy(queue[(queue_head + queue_count++) % QUEUE_LEN], text, sizeof text);
    SDL_UnlockMutex(lock);
}

/* Game thread: queues an unlock's popup; its badge starts downloading on the host thread (cheevos_poll). */
static void popup(const rc_client_achievement_t *a) {
    rt_log("cheevos: unlocked %s (%u)", a->title, a->points);
    SDL_LockMutex(lock);
    if (popup_count < QUEUE_LEN) {
        Popup *p = &popups[(popup_head + popup_count++) % QUEUE_LEN];
        snprintf(p->title, sizeof p->title, "%s", a->title);
        if (rc_client_achievement_get_image_url(a, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, p->url, sizeof p->url) != RC_OK) p->url[0] = 0;
        p->points = a->points;
        p->queued = SDL_GetTicks();
    }
    SDL_UnlockMutex(lock);
}

/* The next unlock's popup, once its badge is in (or waited for long enough) and the last one is over. */
static void poll_popup(Uint64 now) {
    Popup p, waiting[QUEUE_LEN];
    SDL_LockMutex(lock);
    int n = popup_count;
    for (int i = 0; i < n; i++) waiting[i] = popups[(popup_head + i) % QUEUE_LEN];
    SDL_UnlockMutex(lock);
    for (int i = 1; i < n; i++) cheevos_badge(waiting[i].url); /* the later ones' badges download meanwhile */
    if (!n || now < popup_until) return;
    p = waiting[0];
    const uint32_t *badge = p.url[0] ? cheevos_badge(p.url) : NULL; /* also starts its download */
    /* Waited for from when it was queued or the last popup ended, whichever is later. */
    if (!badge && p.url[0] && now - SDL_max(p.queued, popup_until) < POPUP_WAIT_MS) return;
    char line2[48];
    snprintf(line2, sizeof line2, "Achievement unlocked  %u %s", p.points, p.points == 1 ? "pt" : "pts");
    display_popup(badge, CHEEVOS_BADGE, p.title, line2, POPUP_MS);
    popup_until = now + POPUP_MS;
    SDL_LockMutex(lock);
    popup_head = (popup_head + 1) % QUEUE_LEN;
    popup_count--;
    SDL_UnlockMutex(lock);
}

void cheevos_poll(void) {
    Uint64 now = SDL_GetTicks();
    if (!lock) return;
    poll_popup(now);
    if (now < shown_until) return;
    SDL_LockMutex(lock);
    if (queue_count) {
        display_message(queue[queue_head], NOTIFY_MS);
        queue_head = (queue_head + 1) % QUEUE_LEN;
        queue_count--;
        shown_until = now + NOTIFY_MS;
    }
    SDL_UnlockMutex(lock);
}

/* ---- login file ---- */

static void save_login(const char *name, const char *token) {
    FILE *f = fopen(ini_path, "w");
    if (!f) return;
    fprintf(f, "# RetroAchievements login (System Settings > RetroAchievements). To log in without the menu, write\n"
               "#   username = <your name>\n#   password = <your password>\n"
               "# here; the next start logs in and replaces the password with a login token.\n");
    if (name && *name) fprintf(f, "username = %s\n", name);
    if (token && *token) fprintf(f, "token = %s\n", token);
    fclose(f);
}

/* name: sizeof user bytes; password, token: LOGIN_LEN. */
static void load_login(char *name, char *password, char *token) {
    FILE *f = fopen(ini_path, "r");
    if (!f) return;
    char line[512], key[16];
    while (fgets(line, sizeof line, f)) {
        int at = -1;
        if (sscanf(line, " %15[a-z] = %n", key, &at) != 1 || at < 0) continue;
        char *val = line + at;
        size_t len = strcspn(val, "\r\n");
        while (len && (val[len - 1] == ' ' || val[len - 1] == '\t')) len--;
        val[len] = 0;
        char *to = !strcmp(key, "username") ? name : !strcmp(key, "password") ? password : !strcmp(key, "token") ? token : NULL;
        if (to) snprintf(to, to == name ? sizeof user : LOGIN_LEN, "%s", val);
    }
    fclose(f);
}

/* ---- server calls ---- */

typedef struct Request {
    char *url, *post, *content_type;
    rc_client_server_callback_t callback;
    void *callback_data;
} Request;

typedef struct Body { char *data; size_t size; } Body;

static size_t body_write(char *p, size_t size, size_t n, void *userdata) {
    Body *b = userdata;
    char *grown = SDL_realloc(b->data, b->size + size * n + 1);
    if (!grown) return 0;
    memcpy(grown + b->size, p, size * n);
    b->data = grown;
    b->size += size * n;
    b->data[b->size] = 0;
    return size * n;
}

/* A blocking request: GET, or POST when post is given. */
static CURLcode http(const char *url, const char *post, const char *content_type, Body *body, long *status) {
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    CURLcode rc = CURLE_FAILED_INIT;
    *status = 0;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)HTTP_TIMEOUT_S);
#ifdef _WIN32
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA); /* Windows' certificate store */
#endif
        if (post) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
            if (content_type) {
                char header[128];
                snprintf(header, sizeof header, "Content-Type: %s", content_type);
                headers = curl_slist_append(headers, header);
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            }
        }
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
        rc = curl_easy_perform(curl);
        if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    }
    curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
    return rc;
}

static int SDLCALL http_thread(void *arg) {
    Request *r = arg;
    Body body = {0};
    rc_api_server_response_t res = {0};
    long status;
    CURLcode rc = http(r->url, r->post, r->content_type, &body, &status);
    if (rc == CURLE_OK) {
        res.http_status_code = (int)status;
        res.body = body.data ? body.data : "";
        res.body_length = body.size;
    } else {
        res.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        res.body = curl_easy_strerror(rc);
        res.body_length = strlen(res.body);
    }
    r->callback(&res, r->callback_data);
    SDL_free(body.data);
    SDL_free(r->url);
    SDL_free(r->post);
    SDL_free(r->content_type);
    SDL_free(r);
    return 0;
}

static void RC_CCONV server_call(const rc_api_request_t *req, rc_client_server_callback_t callback, void *callback_data,
                                 rc_client_t *c) {
    (void)c;
    Request *r = SDL_calloc(1, sizeof *r);
    rc_api_server_response_t fail = {"out of memory", 13, RC_API_SERVER_RESPONSE_CLIENT_ERROR};
    if (!r) {
        callback(&fail, callback_data);
        return;
    }
    r->url = SDL_strdup(req->url);
    r->post = req->post_data ? SDL_strdup(req->post_data) : NULL;
    r->content_type = req->content_type ? SDL_strdup(req->content_type) : NULL;
    r->callback = callback;
    r->callback_data = callback_data;
    SDL_Thread *t = SDL_CreateThread(http_thread, "cheevos http", r);
    if (t) {
        SDL_DetachThread(t);
        return;
    }
    SDL_free(r->url);
    SDL_free(r->post);
    SDL_free(r->content_type);
    SDL_free(r);
    callback(&fail, callback_data);
}

/* ---- badges ---- */

enum { BADGE_LOADING = 1, BADGE_READY, BADGE_FAILED };
typedef struct Badge {
    char url[192];
    int state; /* BADGE_* */
    uint32_t px[CHEEVOS_BADGE * CHEEVOS_BADGE];
} Badge;
static Badge **badges; /* lock; each Badge stays put, so its pixels can be drawn without the lock once ready */
static int badge_count, badge_cap;
static SDL_AtomicInt fetches;

/* s shrunk to CHEEVOS_BADGE square, each pixel the average of the source pixels it covers. */
static int shrink(SDL_Surface *s, uint32_t *px) {
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);
    if (!c) return 0;
    const int n = CHEEVOS_BADGE;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            int x0 = x * c->w / n, x1 = SDL_max((x + 1) * c->w / n, x0 + 1);
            int y0 = y * c->h / n, y1 = SDL_max((y + 1) * c->h / n, y0 + 1);
            uint32_t sum[4] = {0}, count = 0;
            for (int sy = y0; sy < y1; sy++) {
                const uint32_t *row = (const uint32_t *)((const uint8_t *)c->pixels + sy * c->pitch);
                for (int sx = x0; sx < x1; sx++, count++) {
                    for (int k = 0; k < 4; k++) sum[k] += row[sx] >> (8 * k) & 0xFF;
                }
            }
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) v |= (sum[k] / count) << (8 * k);
            px[y * n + x] = v;
        }
    }
    SDL_DestroySurface(c);
    return 1;
}

/* Reads the badge from the cache, or downloads it into the cache, and shrinks it for the list. */
static int SDLCALL badge_thread(void *arg) {
    char *url = arg, path[1300] = "";
    uint32_t *px = SDL_malloc(sizeof ((Badge *)0)->px);
    int ok = 0;
    const char *name = strrchr(url, '/');
    if (name && name[1] &&
        strspn(name + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.") == strlen(name + 1)) {
        snprintf(path, sizeof path, "%s%s", badge_dir, name + 1); /* a plain file name: cached */
    }
    SDL_Surface *s = path[0] ? SDL_LoadPNG(path) : NULL;
    if (!s) {
        Body body = {0};
        long status;
        if (http(url, NULL, NULL, &body, &status) == CURLE_OK && status == 200 && body.size) {
            s = SDL_LoadPNG_IO(SDL_IOFromConstMem(body.data, body.size), true);
            if (s && path[0]) SDL_SaveFile(path, body.data, body.size);
        }
        SDL_free(body.data);
    }
    if (s && px) ok = shrink(s, px);
    if (s) SDL_DestroySurface(s);
    SDL_LockMutex(lock);
    for (int i = 0; i < badge_count; i++) {
        if (strcmp(badges[i]->url, url)) continue;
        if (ok) memcpy(badges[i]->px, px, sizeof badges[i]->px);
        badges[i]->state = ok ? BADGE_READY : BADGE_FAILED;
        break;
    }
    SDL_UnlockMutex(lock);
    SDL_AddAtomicInt(&fetches, -1);
    SDL_free(px);
    SDL_free(url);
    return 0;
}

const uint32_t *cheevos_badge(const char *image_url) {
    if (!lock || !image_url[0]) return NULL;
    const uint32_t *px = NULL;
    SDL_LockMutex(lock);
    int i = 0;
    while (i < badge_count && strcmp(badges[i]->url, image_url)) i++;
    if (i < badge_count) {
        if (badges[i]->state == BADGE_READY) px = badges[i]->px;
    } else if (SDL_GetAtomicInt(&fetches) < BADGE_FETCHES) { /* else asked for again on a later frame */
        if (badge_count == badge_cap) {
            Badge **grown = SDL_realloc(badges, (size_t)(badge_cap ? badge_cap * 2 : 64) * sizeof *badges);
            if (grown) badges = grown, badge_cap = badge_cap ? badge_cap * 2 : 64;
        }
        Badge *b = SDL_calloc(1, sizeof *b);
        char *url = SDL_strdup(image_url);
        if (b && url && badge_count < badge_cap) {
            snprintf(b->url, sizeof b->url, "%s", image_url);
            b->state = BADGE_LOADING;
            badges[badge_count++] = b;
            SDL_AddAtomicInt(&fetches, 1);
            SDL_Thread *t = SDL_CreateThread(badge_thread, "cheevos badge", url);
            if (t) {
                SDL_DetachThread(t);
            } else {
                b->state = BADGE_FAILED;
                SDL_AddAtomicInt(&fetches, -1);
                SDL_free(url);
            }
        } else {
            SDL_free(b);
            SDL_free(url);
        }
    }
    SDL_UnlockMutex(lock);
    return px;
}

/* ---- rc_client ---- */

/* rcheevos' DS addresses: main RAM from 0, DTCM from DTCM_RA_ADDR. */
static uint32_t RC_CCONV read_memory(uint32_t a, uint8_t *buffer, uint32_t n, rc_client_t *c) {
    (void)c;
    const uint8_t *mem;
    uint32_t size;
    if (a < RT_MAIN_RAM_SIZE) {
        mem = rt_main_ram, size = RT_MAIN_RAM_SIZE;
    } else if (a - DTCM_RA_ADDR < RT_DTCM_SIZE) {
        mem = rt_dtcm, size = RT_DTCM_SIZE, a -= DTCM_RA_ADDR;
    } else {
        return 0;
    }
    n = SDL_min(n, size - a);
    memcpy(buffer, mem + a, n);
    return n;
}

static void RC_CCONV log_message(const char *message, const rc_client_t *c) {
    (void)c;
    rt_log("rcheevos: %s", message);
}

static void RC_CCONV game_loaded(int result, const char *error, rc_client_t *c, void *userdata) {
    (void)userdata;
    if (result != RC_OK) {
        notify("RetroAchievements: %s", result == RC_NO_GAME_LOADED ? "this ROM is not in its list" : error ? error : rc_error_str(result));
        return;
    }
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(c, &s);
    if (!s.num_core_achievements) notify("RetroAchievements: no achievements yet");
    else notify("Achievements: %u of %u unlocked", s.num_unlocked_achievements, s.num_core_achievements);
}

static void RC_CCONV logged_in(int result, const char *error, rc_client_t *c, void *userdata) {
    (void)userdata;
    const rc_client_user_t *u = rc_client_get_user_info(c);
    if (result != RC_OK || !u) {
        SDL_LockMutex(lock);
        /* The server refused the login: keep only the name, so the next start does not try it again. */
        if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED) save_login(user, NULL);
        user[0] = 0;
        SDL_UnlockMutex(lock);
        SDL_SetAtomicInt(&state, CHEEVOS_LOGGED_OUT);
        notify("RetroAchievements: %s", result == RC_INVALID_CREDENTIALS ? "wrong name or password"
                                        : result == RC_EXPIRED_TOKEN     ? "login expired, log in again"
                                        : error                          ? error
                                                                         : rc_error_str(result));
        return;
    }
    SDL_LockMutex(lock);
    snprintf(user, sizeof user, "%s", u->display_name);
    save_login(u->username, u->token);
    SDL_UnlockMutex(lock);
    SDL_SetAtomicInt(&state, CHEEVOS_LOGGED_IN);
    notify("RetroAchievements: logged in as %s", u->display_name);
    if (!rc_client_is_game_loaded(c)) {
        rc_client_begin_identify_and_load_game(c, RC_CONSOLE_NINTENDO_DS, rt_rom.path, NULL, 0, game_loaded, NULL);
    }
}

/* Game thread, inside rc_client_do_frame. */
static void RC_CCONV event(const rc_client_event_t *e, rc_client_t *c) {
    switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        popup(e->achievement);
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED: {
        const rc_client_game_t *g = rc_client_get_game_info(c);
        notify("Every achievement of %s unlocked!", g ? g->title : "the game");
        break;
    }
    case RC_CLIENT_EVENT_SERVER_ERROR:
        notify("RetroAchievements: %s", e->server_error->error_message);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED: notify("RetroAchievements: offline, retrying"); break;
    case RC_CLIENT_EVENT_RECONNECTED: notify("RetroAchievements: back online"); break;
    default: break;
    }
}

void cheevos_init(void) {
    lock = SDL_CreateMutex();
    if (!lock || curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return;
    client = rc_client_create(read_memory, server_call);
    if (!client) return;
    rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_WARN, log_message);
    rc_client_set_event_handler(client, event);
    rc_client_set_hardcore_enabled(client, 0);
    rc_client_set_allow_background_memory_reads(client, 0); /* memory is read on the game thread only */
    int n = snprintf(user_agent, sizeof user_agent, "EggOfTime/%s (%s) ", EGG_VERSION, SDL_GetPlatform());
    rc_client_get_user_agent_clause(client, user_agent + n, sizeof user_agent - (size_t)n);

    snprintf(ini_path, sizeof ini_path, "%sretroachievements.ini", rt_data_dir());
    snprintf(badge_dir, sizeof badge_dir, "%scache/badges/", rt_data_dir());
    SDL_CreateDirectory(badge_dir);
    char password[LOGIN_LEN] = "", token[LOGIN_LEN] = "";
    load_login(user, password, token);
    if (!user[0] || (!password[0] && !token[0])) {
        user[0] = 0;
        return;
    }
    SDL_SetAtomicInt(&state, CHEEVOS_LOGGING_IN);
    if (password[0]) rc_client_begin_login_with_password(client, user, password, logged_in, NULL);
    else rc_client_begin_login_with_token(client, user, token, logged_in, NULL);
    SDL_memset(password, 0, sizeof password);
}

void cheevos_frame(void) {
    if (client) rc_client_do_frame(client);
}

void cheevos_reset(void) {
    if (client) rc_client_reset(client);
}

int cheevos_state(void) { return SDL_GetAtomicInt(&state); }

const char *cheevos_user(void) {
    static char copy[64];
    SDL_LockMutex(lock);
    snprintf(copy, sizeof copy, "%s", SDL_GetAtomicInt(&state) == CHEEVOS_LOGGED_IN ? user : "");
    SDL_UnlockMutex(lock);
    return copy;
}

void cheevos_login(const char *name, const char *password) {
    if (!client || !*name || !*password || !SDL_CompareAndSwapAtomicInt(&state, CHEEVOS_LOGGED_OUT, CHEEVOS_LOGGING_IN)) return;
    SDL_LockMutex(lock);
    snprintf(user, sizeof user, "%s", name);
    SDL_UnlockMutex(lock);
    rc_client_begin_login_with_password(client, name, password, logged_in, NULL);
}

int cheevos_list(const CheevosItem **items) {
    static CheevosItem *list;
    static int count, cap;
    static Uint64 refreshed_at;
    Uint64 now = SDL_GetTicks();
    if (client && (!refreshed_at || now - refreshed_at >= LIST_REFRESH_MS)) {
        refreshed_at = now;
        count = 0;
        /* rcheevos locks its state while copying, so the game thread may go on unlocking meanwhile. */
        rc_client_achievement_list_t *l = rc_client_create_achievement_list(
            client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
        for (int unlocked = 1; l && unlocked >= 0; unlocked--) {
            for (uint32_t b = 0; b < l->num_buckets; b++) {
                for (uint32_t i = 0; i < l->buckets[b].num_achievements; i++) {
                    const rc_client_achievement_t *a = l->buckets[b].achievements[i];
                    if (a->id >= WARNING_ID || !(a->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE) != !unlocked) continue;
                    if (count == cap) {
                        CheevosItem *grown = SDL_realloc(list, (size_t)(cap ? cap * 2 : 64) * sizeof *list);
                        if (!grown) goto done;
                        list = grown, cap = cap ? cap * 2 : 64;
                    }
                    CheevosItem *it = &list[count++];
                    snprintf(it->title, sizeof it->title, "%s", a->title ? a->title : "");
                    snprintf(it->description, sizeof it->description, "%s", a->description ? a->description : "");
                    snprintf(it->progress, sizeof it->progress, "%s", unlocked ? "" : a->measured_progress);
                    int image = unlocked ? RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED : RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE;
                    if (rc_client_achievement_get_image_url(a, image, it->image_url, sizeof it->image_url) != RC_OK) {
                        it->image_url[0] = 0;
                    }
                    it->points = a->points;
                    it->unlocked = unlocked;
                }
            }
        }
    done:
        if (l) rc_client_destroy_achievement_list(l);
    }
    *items = list;
    return count;
}

void cheevos_logout(void) {
    if (!client || !SDL_CompareAndSwapAtomicInt(&state, CHEEVOS_LOGGED_IN, CHEEVOS_LOGGED_OUT)) return;
    rc_client_logout(client); /* also unloads the game */
    SDL_LockMutex(lock);
    user[0] = 0;
    save_login(NULL, NULL);
    SDL_UnlockMutex(lock);
    notify("RetroAchievements: logged out");
}
