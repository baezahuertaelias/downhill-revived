/* dhd.ini on the Switch: port/render/settings's rules in C (dhd_settings.h), where the Rust library
   can't run. The same table of keys and player defaults; the choices' labels stay in
   port/hle/settings_table.h, which the game core reads. */
#include "dhd_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include "settings_table.h"

typedef struct { const char *key, *env, *def; int always; } Setting;
static const Setting settings[] = {
    {"iso", "DHD_ISO", "", 0}, {"scale", "DHD_SCALE", "2", 0}, {"window", "DHD_WINDOW", "borderless", 0},
    {"size", "", "monitor", 0}, {"width", "DHD_WINDOW_WIDTH", "monitor", 0}, {"height", "DHD_WINDOW_HEIGHT", "monitor", 0},
    {"monitor", "DHD_MONITOR", "", 0}, {"widescreen", "DHD_WIDESCREEN", "1", 0}, {"DHD_ASPECT", "DHD_ASPECT", "wide", 0},
    {"DHD_PRESENT_FILTER", "DHD_PRESENT_FILTER", "linear", 0}, {"renderer", "DHD_RENDER", "native", 0},
    {"skip_movies", "DHD_SKIP_MOVIES", "1", 0}, {"vibration", "DHD_VIBRATION", "1", 0}, {"textures", "DHD_TEXTURES", "on", 0},
    {"texture_pack", "DHD_TEXTURE_PACK", "", 0}, {"texture_cache_mb", "DHD_TEXTURE_CACHE_MB", "", 0}, {"font", "DHD_FONT", "sharp", 0},
    {"fps", "DHD_FPS", "60", 0}, {"vsync", "DHD_VSYNC", "1", 0}, {"perf_counter", "DHD_PERF_COUNTER", "0", 0},
    {"unlock_all", "DHD_UNLOCK_ALL", "0", 0}, {"memory_card", "DHD_MEMCARD", "", 0}, {"sound_latency_ms", "DHD_SND_LATENCY_MS", "", 0},
    {"server", "DHD_NET_SERVER", "wss://dhd.skuza.app", 1}, {"name", "DHD_NET_NAME", "", 0}, {"rider", "DHD_NET_RIDER", "", 0},
    {"auto_update", "", "ask", 0}, {"update_url", "DHD_UPDATE_URL", "", 0}, {"discord", "DHD_DISCORD", "on", 0}, {"game_options", "", "", 0},
    /* what main's table has gained since 0.7.6 (table.rs): the optional effects (finding 183, Performance
       Mode turns them off), the stick's dead zone, the room's region, the launcher's language.
       native_vu is off here: dhd_render_nx.c has no mesh draws yet (finding 270). */
    {"shadows", "DHD_SHADOWS", "1", 0}, {"lens_flare", "DHD_LENS_FLARE", "1", 0}, {"particles", "DHD_PARTICLES", "1", 0},
    {"native_vu", "DHD_NATIVE_VU1", "off", 0}, {"stick_dead_zone", "DHD_STICK_DEAD_ZONE", "game", 0},
    {"room_region", "DHD_NET_ROOM_REGION", "auto", 0}, {"language", "", "", 0},
};
enum { COUNT = sizeof settings / sizeof *settings };

static const Setting *find(const char *key)
{
    for (int i = 0; i < COUNT; i++)
        if (!strcasecmp(settings[i].key, key) || (*settings[i].env && !strcasecmp(settings[i].env, key))) return &settings[i];
    return NULL;
}

/* table.rs's canonical: a value as the game reads it, a choice's own spelling and a switch's off and
   on words (atoi("on") is 0, so "vibration = on" would have been off). Keeps value otherwise. */
static const char *canonical(const Setting *s, const char *value)
{
    static const char *const switches[] = {"widescreen", "skip_movies", "vibration", "textures", "font", "vsync", "perf_counter", "unlock_all", "discord", "shadows", "lens_flare", "particles"};
    static const char *const off_words[] = {"0", "off", "false", "no"}, *const on_words[] = {"1", "on", "true", "yes"};
    char v[64];
    while (*value == ' ' || *value == '\t') value++;
    size_t n = strlen(value);
    while (n && (value[n - 1] == ' ' || value[n - 1] == '\t')) n--;
    if (n >= sizeof v) return value;
    memcpy(v, value, n);
    v[n] = 0;
    const SettingInfo *info = NULL;
    for (size_t i = 0; i < sizeof setting_table / sizeof *setting_table; i++)
        if (!strcmp(setting_table[i].key, s->key)) info = &setting_table[i];
    if (!info) return value;
    for (int i = 0; i < info->count; i++)
        if (!strcasecmp(info->choices[i].value, v)) return info->choices[i].value;
    int is_switch = 0;
    for (size_t i = 0; i < sizeof switches / sizeof *switches; i++) is_switch |= !strcmp(switches[i], s->key);
    if (is_switch)
        for (int i = 0; i < 4; i++) {
            if (!strcasecmp(off_words[i], v)) return info->choices[0].value;
            if (!strcasecmp(on_words[i], v)) return info->choices[1].value;
        }
    if (!*v || !strcmp(s->key, "widescreen")) return value;  /* widescreen may be a shape: 21:9 */
    if (is_switch) return info->choices[1].value;
    return !strcmp(s->key, "renderer") ? "exact" : value;
}

static int off(const char *path) { return !strcasecmp(path, "none") || !strcmp(path, "0"); }

static char *read_all(const char *path)
{
    FILE *f = off(path) ? NULL : fopen(path, "rb");
    if (!f && !off(path)) {  /* a save cut off between its remove and its rename (dhd_settings_save) */
        char temp[1200];
        snprintf(temp, sizeof temp, "%s.new", path);
        f = fopen(temp, "rb");
    }
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)n + 1);
    if (text) text[fread(text, 1, (size_t)n, f)] = 0;
    fclose(f);
    return text;
}

/* A line's key and value (ini.rs's entry); 0 for a comment, a section or a blank line. */
static int entry(const char *line, size_t n, char *key, size_t ks, char *value, size_t vs)
{
    const char *s = line, *end = line + n;
    if (n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) s += 3;
    while (s < end && strchr(" \t\r", *s)) s++;
    while (end > s && strchr(" \t\r", end[-1])) end--;
    if (s == end || strchr(";#[", *s)) return 0;
    const char *eq = memchr(s, '=', (size_t)(end - s));
    if (!eq) return 0;
    const char *ke = eq;
    while (ke > s && strchr(" \t", ke[-1])) ke--;
    if (ke == s || (size_t)(ke - s) >= ks) return 0;
    memcpy(key, s, (size_t)(ke - s));
    key[ke - s] = 0;
    const char *v = eq + 1, *ve = end;
    while (v < ve && strchr(" \t", *v)) v++;
    const char *close = v < ve && *v == '"' ? memchr(v + 1, '"', (size_t)(ve - v - 1)) : NULL;
    if (close) v++, ve = close;
    else {
        for (const char *p = v; p < ve; p++)
            if ((*p == ';' || *p == '#') && (p == v || p[-1] == ' ' || p[-1] == '\t')) { ve = p; break; }
        while (ve > v && strchr(" \t", ve[-1])) ve--;
    }
    size_t len = (size_t)(ve - v) < vs - 1 ? (size_t)(ve - v) : vs - 1;
    memcpy(value, v, len);
    value[len] = 0;
    return 1;
}

static int written(const char *text, const char *key, char *out, size_t size)
{
    const Setting *s = find(key);
    char k[128], v[1024];
    for (const char *p = text; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (entry(p, n, k, sizeof k, v, sizeof v) && *v &&
            (s ? (!strcasecmp(k, s->key) || (*s->env && !strcasecmp(k, s->env))) : !strcasecmp(k, key))) {
            snprintf(out, size, "%s", v);
            return 1;
        }
        p = nl ? nl + 1 : NULL;
    }
    return 0;
}

int dhd_settings_load(const char *path, DhdSettingsGet get, DhdSettingsSet set, DhdSettingsUnknown unknown)
{
    char *text = read_all(path), v[1024], k[128], overrides[1024] = ",";
    for (int i = 0; i < COUNT; i++)
        if (*settings[i].env && get(settings[i].env) && strlen(overrides) + strlen(settings[i].env) + 2 < sizeof overrides) {
            strcat(overrides, settings[i].env);
            strcat(overrides, ",");
        }
    set("DHD_ENV_OVERRIDES", overrides);
    int size_free = !get("DHD_WINDOW_WIDTH") && !get("DHD_WINDOW_HEIGHT") &&
                    !(text && (written(text, "width", v, sizeof v) || written(text, "height", v, sizeof v)));
    for (int i = 0; i < COUNT; i++) {
        const Setting *s = &settings[i];
        if (!*s->env || get(s->env)) continue;
        int side = !strcmp(s->key, "width") || !strcmp(s->key, "height");
        if (text && written(text, s->key, v, sizeof v)) set(s->env, canonical(s, v));
        else if ((text || s->always) && *s->def && (!side || size_free)) set(s->env, s->def);
    }
    for (const char *p = text; p && *p;) {  /* any DHD_* line sets that variable */
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (entry(p, n, k, sizeof k, v, sizeof v)) {
            int dhd = !strncasecmp(k, "DHD_", 4);
            if (dhd && *v && !find(k) && !get(k)) set(k, v);
            if (!dhd && !find(k) && unknown) unknown(k);
        }
        p = nl ? nl + 1 : NULL;
    }
    int had = text != NULL;
    free(text);
    return had;
}

static void save_one(FILE *f, const char *text, const char *key, const char *value)
{
    const Setting *s = find(key);
    char k[128], v[1024], line[1200];
    int done = 0;
    if (s) key = s->key;  /* one spelling, as ini.rs's set */
    const char *quote = strpbrk(value, ";#\"") || *value == ' ' || *value == '\t' ||
                        (*value && strchr(" \t", value[strlen(value) - 1])) ? "\"" : "";
    if (*value) snprintf(line, sizeof line, "%s = %s%s%s\n", key, quote, value, quote);
    else snprintf(line, sizeof line, "%s =\n", key);
    for (const char *p = text; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (entry(p, n, k, sizeof k, v, sizeof v) &&
            (s ? (!strcasecmp(k, s->key) || (*s->env && !strcasecmp(k, s->env))) : !strcasecmp(k, key))) {
            if (!done) fputs(line, f);
            done = 1;
        } else {
            fwrite(p, 1, n, f);
            fputc('\n', f);
        }
        p = nl ? nl + 1 : NULL;
    }
    if (!done) fputs(line, f);
}

int dhd_settings_save(const char *path, const char *key, const char *value)
{
    if (off(path)) return 0;
    if (strchr(value, '\n') || strchr(value, '\r')) return 0;
    if (!strcasecmp(key, "size")) {  /* the window size is the pair's, width and height (ini.rs's set) */
        char w[32] = "monitor", h[32] = "monitor";
        if (strcasecmp(value, "monitor") && sscanf(value, "%31[0-9]%*[xX]%31[0-9]", w, h) != 2) return 0;
        return dhd_settings_save(path, "width", w) && dhd_settings_save(path, "height", h);
    }
    char *text = read_all(path), temp[1200];
    snprintf(temp, sizeof temp, "%s.new", path);
    FILE *f = fopen(temp, "wb");
    if (!f) { free(text); return 0; }
    if (!text) fputs("; Downhill Revived settings: one \"key = value\" per line.\n", f);
    save_one(f, text, key, value);
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;  /* as lib.rs's sync_all */
    ok = fclose(f) == 0 && ok;
    free(text);
    /* FAT's rename won't replace a file, as the POSIX one lib.rs uses does: read_all finds the .new
       if the end comes in between. */
    if (ok) { remove(path); ok = rename(temp, path) == 0; }
    if (!ok) remove(temp);
    return ok;
}

int dhd_settings_read(const char *path, const char *key, char *out, size_t size)
{
    char *text = read_all(path);
    int got = text && written(text, key, out, size);
    free(text);
    return got;
}
