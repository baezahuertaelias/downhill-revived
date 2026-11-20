/* dhd.ini on the Switch: port/render/settings's rules in C (dhd_settings.h), where the Rust library
   can't run. The same table of keys and player defaults; the choices' labels stay in
   port/hle/settings_table.h, which the game core reads. */
#include "dhd_settings.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <switch.h>
#include "settings_table.h"
#include "controls_table.h"

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
    /* not settings, but dhd.ini's own lines: the install id (dhd_settings_install_id) and the
       bindings (read by dhd_controls_layouts) */
    {"install_id", "", "", 0}, {"keyboard", "DHD_KEYBOARD", "", 0}, {"pad1", "DHD_PAD1", "", 0},
    {"pad2", "DHD_PAD2", "", 0}, {"pad3", "DHD_PAD3", "", 0}, {"pad4", "DHD_PAD4", "", 0},
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

/* The install id (finding 161): dhd.ini's install_id when it is 32 lowercase hex digits, else a new
   random one saved there (without the file: one for this run only). */
int dhd_settings_install_id(const char *path, char *out, size_t size)
{
    static char run_id[33];
    char id[64];
    if (size < 33) return 0;
    if (dhd_settings_read(path, "install_id", id, sizeof id) && strlen(id) == 32 && strspn(id, "0123456789abcdef") == 32) {
        snprintf(out, size, "%s", id);
        return 1;
    }
    if (!*run_id) {
        uint8_t bytes[16];
        randomGet(bytes, sizeof bytes);
        for (int i = 0; i < 16; i++) snprintf(run_id + 2 * i, 3, "%02x", bytes[i]);
    }
    snprintf(out, size, "%s", run_id);
    char *text = off(path) ? NULL : read_all(path);
    if (text) dhd_settings_save(path, "install_id", run_id);  /* only into a dhd.ini that is there */
    free(text);
    return 1;
}

/* ---- the bindings: port/render/settings/src/controls.rs's rules in C (finding 157; on the Switch,
   finding 270). The same dhd.ini lines, layouts and CONTROLS page words; keep it in step with
   controls.rs (its tests say what each rule does). ---- */

typedef struct { const char *p; size_t n; } Span;  /* a piece of a line, not NUL-terminated */

static int span_is(Span s, const char *word) { return strlen(word) == s.n && !memcmp(s.p, word, s.n); }

/* The next piece of *rest up to one of seps (empty pieces count, as Rust's split); 0 at the end.
   seps NULL: split at white space, skipping empty pieces, as Rust's split_whitespace. */
static int next_piece(Span *rest, const char *seps, Span *out)
{
    if (!seps) {
        while (rest->n && isspace((unsigned char)*rest->p)) rest->p++, rest->n--;
        if (!rest->n) return 0;
        size_t n = 0;
        while (n < rest->n && !isspace((unsigned char)rest->p[n])) n++;
        *out = (Span){rest->p, n};
        rest->p += n, rest->n -= n;
        return 1;
    }
    if (!rest->p) return 0;
    size_t n = 0;
    while (n < rest->n && !strchr(seps, rest->p[n])) n++;
    *out = (Span){rest->p, n};
    if (n < rest->n) rest->p += n + 1, rest->n -= n + 1;
    else rest->p = NULL, rest->n = 0;
    return 1;
}

static int is_pad(int device) { return device != CONTROL_KEYBOARD; }

static int target_named(Span name)
{
    for (int t = 0; t < CONTROL_TARGETS; t++)
        if (span_is(name, control_targets[t].name)) return t;
    return -1;
}

/* Whether target can be bound on device (a controller's sticks are its own). */
static int bindable(int device, int target)
{
    return target >= 0 && target < CONTROL_TARGETS && (!is_pad(device) || target < CONTROL_BUTTONS);
}

/* The input code a layout of device keeps for the dhd.ini name (vk or id); 0 none. */
static int input_named(int device, Span name)
{
    if (is_pad(device)) {
        for (int i = 0; i < CONTROL_PAD_INPUTS; i++)
            if (span_is(name, control_pad_inputs[i].name)) return control_pad_inputs[i].id;
    } else {
        for (int i = 0; i < CONTROL_KEYS; i++)
            if (span_is(name, control_keys[i].name)) return control_keys[i].vk;
    }
    return 0;
}

/* A code's dhd.ini name (label 0) or the pages' word for it (label 1; a controller's by its kind,
   PAD_KIND_*: 1 PlayStation, 3 Nintendo, else Xbox's words); "?" for none. */
static const char *input_word(int device, int code, int label, int kind)
{
    if (is_pad(device)) {
        for (int i = 0; i < CONTROL_PAD_INPUTS; i++)
            if (control_pad_inputs[i].id == code)
                return !label ? control_pad_inputs[i].name : control_pad_inputs[i].label[kind == 1 ? 1 : kind == 3 ? 2 : 0];
    } else {
        for (int i = 0; i < CONTROL_KEYS; i++)
            if (control_keys[i].vk == code) return label ? control_keys[i].label : control_keys[i].name;
    }
    return "?";
}

static int has(const uint8_t slots[CONTROL_SLOTS], int code) { return slots[0] == code || slots[1] == code; }

/* An alternate without a main becomes the main. */
static void tidy(uint8_t slots[CONTROL_SLOTS])
{
    if (!slots[0]) slots[0] = slots[1], slots[1] = 0;
}

static const struct { uint8_t bit; const char *name; } invert_names[] = {
    {CONTROL_INVERT_LX, "lx"}, {CONTROL_INVERT_LY, "ly"}, {CONTROL_INVERT_RX, "rx"}, {CONTROL_INVERT_RY, "ry"}};
static const struct { uint8_t bit; const char *name; } swap_names[] = {{CONTROL_SWAP_L, "l1l2"}, {CONTROL_SWAP_R, "r1r2"}};

/* A problem with device's line, "pad1: ..." */
static void say(DhdSettingsUnknown problem, int device, const char *format, ...) __attribute__((format(printf, 3, 4)));
static void say(DhdSettingsUnknown problem, int device, const char *format, ...)
{
    if (!problem) return;
    char line[600];
    int n = snprintf(line, sizeof line, "%s: ", control_devices[device].key);
    va_list args;
    va_start(args, format);
    vsnprintf(line + n, sizeof line - (size_t)n, format, args);
    va_end(args);
    problem(line);
}

/* The layout a line of device gives (controls.rs's parse): an entry that can't be read is skipped,
   with a line through problem, and its button keeps its default. */
static void parse(int device, const char *text, ControlsLayout *l, DhdSettingsUnknown problem)
{
    int pad = is_pad(device), named[CONTROL_TARGETS] = {0};
    *l = control_defaults[device];
    Span rest = {text, strlen(text)}, entry, v;
    while (next_piece(&rest, NULL, &entry)) {
        const char *colon = memchr(entry.p, ':', entry.n);
        if (!colon) {
            say(problem, device, "\"%.*s\" is not button:input", (int)entry.n, entry.p);
            continue;
        }
        Span name = {entry.p, (size_t)(colon - entry.p)}, values = {colon + 1, entry.n - (size_t)(colon - entry.p) - 1};
        int swap = -1;
        for (int i = 0; pad && i < 2; i++)
            if (span_is(name, swap_names[i].name)) swap = i;
        if (pad && span_is(name, "sticks")) {
            if (span_is(values, "swap")) l->swap_sticks = 1;
            else if (span_is(values, "normal")) l->swap_sticks = 0;
            else say(problem, device, "sticks:%.*s is not sticks:swap", (int)values.n, values.p);
            continue;
        }
        if (swap >= 0) {
            if (span_is(values, "swap")) l->swap_shoulders |= swap_names[swap].bit;
            else if (span_is(values, "normal")) l->swap_shoulders &= (uint8_t)~swap_names[swap].bit;
            else say(problem, device, "%s:%.*s is not %s:swap", swap_names[swap].name, (int)values.n, values.p, swap_names[swap].name);
            continue;
        }
        if (pad && span_is(name, "invert")) {
            l->invert = 0;
            for (Span r = values; next_piece(&r, ",", &v);) {
                if (!v.n || span_is(v, "none")) continue;
                int found = 0;
                for (int i = 0; i < 4 && !found; i++)
                    if (span_is(v, invert_names[i].name)) l->invert |= invert_names[i].bit, found = 1;
                if (!found) say(problem, device, "invert:%.*s is not lx, ly, rx or ry", (int)v.n, v.p);
            }
            continue;
        }
        int t = target_named(name);
        if (!bindable(device, t)) {
            say(problem, device, "%.*s is not a button %s", (int)name.n, name.p, pad ? "of a controller" : "of the keyboard");
            continue;
        }
        if (named[t]) {
            say(problem, device, "%.*s is named twice: the first one counts", (int)name.n, name.p);
            continue;
        }
        uint8_t slots[CONTROL_SLOTS] = {0};
        int n = 0, readable = 0;
        for (Span r = values; next_piece(&r, ",", &v);) {
            if (!v.n || span_is(v, "none") || span_is(v, "-")) continue;
            readable = 1;
            int code = input_named(device, v);
            if (!code) {
                say(problem, device, "%.*s: \"%.*s\" is not %s", (int)name.n, name.p, (int)v.n, v.p,
                    pad ? "a controller's button" : "a key the game can use");
                continue;
            }
            if (has(slots, code)) continue;
            int other = -1;
            for (int u = 0; u < CONTROL_TARGETS && other < 0; u++)
                if (named[u] && has(l->input[u], code)) other = u;
            if (other >= 0) {
                say(problem, device, "%.*s: %.*s is %s's already", (int)name.n, name.p, (int)v.n, v.p, control_targets[other].label);
                continue;
            }
            if (n == CONTROL_SLOTS) {
                say(problem, device, "%.*s: two inputs at most, %.*s left out", (int)name.n, name.p, (int)v.n, v.p);
                continue;
            }
            slots[n++] = (uint8_t)code;
        }
        /* `none` (or nothing) unbinds the button; a value the game can't read is skipped, so a
           button with none it can read keeps its default (PLAYING.md, "Controls"). */
        if (!n && readable) continue;
        memcpy(l->input[t], slots, sizeof slots);
        named[t] = 1;
    }
    /* a default the line gave another button leaves the button the line doesn't name */
    for (int t = 0; t < CONTROL_TARGETS; t++) {
        if (named[t]) continue;
        for (int s = 0; s < CONTROL_SLOTS; s++)
            for (int u = 0; u < CONTROL_TARGETS && l->input[t][s]; u++)
                if (named[u] && has(l->input[u], l->input[t][s])) l->input[t][s] = 0;
        tidy(l->input[t]);
    }
}

/* Appends word to out (size bytes), a space before it unless out is empty. */
static void add(char *out, size_t size, const char *word)
{
    size_t n = strlen(out);
    if (n + 1 < size) snprintf(out + n, size - n, "%s%s", n ? " " : "", word);
}

/* The line for a layout of device (controls.rs's format): the buttons that differ from the
   defaults, then a controller's stick and shoulder options; "" for the defaults. */
static void format_layout(int device, const ControlsLayout *l, char *out, size_t size)
{
    char entry[64];
    *out = 0;
    for (int t = 0; t < CONTROL_TARGETS; t++) {
        if (!memcmp(l->input[t], control_defaults[device].input[t], CONTROL_SLOTS)) continue;
        int n = snprintf(entry, sizeof entry, "%s:", control_targets[t].name), any = 0;
        for (int s = 0; s < CONTROL_SLOTS; s++)
            if (l->input[t][s])
                n += snprintf(entry + n, sizeof entry - (size_t)n, "%s%s", any++ ? "," : "", input_word(device, l->input[t][s], 0, 0));
        if (!any) snprintf(entry + n, sizeof entry - (size_t)n, "none");
        add(out, size, entry);
    }
    if (!is_pad(device)) return;
    if (l->swap_sticks) add(out, size, "sticks:swap");
    if (l->invert) {
        int n = snprintf(entry, sizeof entry, "invert:"), any = 0;
        for (int i = 0; i < 4; i++)
            if (l->invert & invert_names[i].bit)
                n += snprintf(entry + n, sizeof entry - (size_t)n, "%s%s", any++ ? "," : "", invert_names[i].name);
        add(out, size, entry);
    }
    for (int i = 0; i < 2; i++)
        if (l->swap_shoulders & swap_names[i].bit) {
            snprintf(entry, sizeof entry, "%s:swap", swap_names[i].name);
            add(out, size, entry);
        }
}

/* The first essential target the layout has nothing on; -1 none. */
static int missing(const ControlsLayout *l)
{
    for (int t = 0; t < CONTROL_TARGETS; t++)
        if (control_targets[t].essential && !l->input[t][0]) return t;
    return -1;
}

/* Every device's layout (controls.rs's Controls::from_values): a hand-made line that leaves an
   essential button with nothing on its device puts that device alone back to its defaults. */
static void from_values(const char *const values[DHD_CONTROLS_DEVICES], ControlsLayout out[CONTROL_DEVICES],
                        DhdSettingsUnknown problem)
{
    for (int d = 0; d < CONTROL_DEVICES; d++) parse(d, values && values[d] ? values[d] : "", &out[d], problem);
    for (int d = 0; d < CONTROL_DEVICES; d++) {
        int t = missing(&out[d]);
        if (t < 0) continue;
        say(problem, d, "%s has no %s: %s is back to its defaults", control_targets[t].label, is_pad(d) ? "button" : "key",
            control_devices[d].label);
        out[d] = control_defaults[d];
    }
}

void dhd_controls_layouts(const char *const values[DHD_CONTROLS_DEVICES], DhdControlsLayout out[DHD_CONTROLS_DEVICES],
                          DhdSettingsUnknown problem)
{
    _Static_assert(sizeof(DhdControlsLayout) == sizeof(ControlsLayout), "a layout is 52 bytes");
    ControlsLayout layouts[CONTROL_DEVICES];
    from_values(values, layouts, problem);
    if (out) memcpy(out, layouts, sizeof layouts);
}

/* controls.rs's Controls::edit through c.rs's dhd_controls_edit: the same answers and words. */
int dhd_controls_edit(const char *const values[DHD_CONTROLS_DEVICES], int device, int edit, int target, int slot, int input,
                      int kind, char *value, size_t value_size, char *message, size_t message_size)
{
    ControlsLayout now[CONTROL_DEVICES], next;
    char said[160] = "";
    if (device < 0 || device >= CONTROL_DEVICES || target < 0 || slot < 0 || edit < CONTROL_ASSIGN || edit > CONTROL_SWAP_SHOULDERS)
        return CONTROL_UNCHANGED;
    if (input < 0 || input > 255) input = 0;
    from_values(values, now, NULL);
    next = now[device];
    ControlsLayout *l = &next;
    int pad = is_pad(device), done = CONTROL_CHANGED;
    switch (edit) {
    case CONTROL_ASSIGN: {
        if (!bindable(device, target) || slot >= CONTROL_SLOTS) {
            char dev[32];
            snprintf(dev, sizeof dev, "%s", control_devices[device].label);
            for (char *c = dev; *c; c++) *c = (char)tolower((unsigned char)*c);
            snprintf(said, sizeof said, "%s can't be set on the %s", target < CONTROL_TARGETS ? control_targets[target].label : "?", dev);
            done = CONTROL_REFUSED;
            break;
        }
        const char *word = input_word(device, input, 1, kind);
        if (!strcmp(word, "?") || !input) {
            snprintf(said, sizeof said, "%s", pad ? "That button can't be used" : "That key can't be used");
            done = CONTROL_REFUSED;
            break;
        }
        if (!l->input[target][0]) slot = 0;  /* an alternate without a main is the main */
        int ht = -1, hs = -1;
        for (int t = 0; t < CONTROL_TARGETS && ht < 0; t++)
            for (int s = 0; s < CONTROL_SLOTS && ht < 0; s++)
                if (l->input[t][s] == input) ht = t, hs = s;
        if (ht == target && hs == slot) done = CONTROL_UNCHANGED;
        else if (ht == target) {  /* the button's other slot: the two change places */
            uint8_t first = l->input[ht][0];
            l->input[ht][0] = l->input[ht][1], l->input[ht][1] = first;
            tidy(l->input[ht]);
        } else if (ht >= 0) {
            uint8_t old = l->input[target][slot];
            l->input[target][slot] = (uint8_t)input;
            l->input[ht][hs] = old;
            tidy(l->input[ht]);
            if (old) snprintf(said, sizeof said, "Swapped %s and %s", control_targets[target].label, control_targets[ht].label);
            else snprintf(said, sizeof said, "%s moved from %s to %s", word, control_targets[ht].label, control_targets[target].label);
        } else l->input[target][slot] = (uint8_t)input;
        break;
    }
    case CONTROL_CLEAR:
        if (!bindable(device, target) || slot >= CONTROL_SLOTS || !l->input[target][slot]) done = CONTROL_UNCHANGED;
        else {
            l->input[target][slot] = 0;
            tidy(l->input[target]);
        }
        break;
    case CONTROL_RESET: *l = control_defaults[device]; break;
    case CONTROL_SWAP_STICKS:
        if (pad) l->swap_sticks = input != 0;
        else done = CONTROL_UNCHANGED;
        break;
    default: {  /* CONTROL_INVERT, CONTROL_SWAP_SHOULDERS: target is the bit */
        int bit = target > 255 ? 0 : target, known = 0;
        if (edit == CONTROL_INVERT)
            for (int i = 0; i < 4; i++) known |= invert_names[i].bit == bit;
        else
            for (int i = 0; i < 2; i++) known |= swap_names[i].bit == bit;
        uint8_t *bits = edit == CONTROL_INVERT ? &l->invert : &l->swap_shoulders;
        if (pad && known) *bits = input ? (uint8_t)(*bits | bit) : (uint8_t)(*bits & ~bit);
        else done = CONTROL_UNCHANGED;
        break;
    }
    }
    if (done == CONTROL_CHANGED && !memcmp(&next, &now[device], sizeof next)) done = CONTROL_UNCHANGED;
    if (done == CONTROL_CHANGED) {
        int t = missing(&next);
        if (t >= 0) {
            snprintf(said, sizeof said, "%s needs %s", control_targets[t].label, pad ? "a button" : "a key");
            done = CONTROL_REFUSED;
        } else now[device] = next;
    }
    if (done == CONTROL_UNCHANGED) *said = 0;
    if (value && value_size) format_layout(device, &now[device], value, value_size);
    if (message && message_size) snprintf(message, message_size, "%s", said);
    return done;
}

int dhd_settings_read(const char *path, const char *key, char *out, size_t size)
{
    char *text = read_all(path);
    int got = text && written(text, key, out, size);
    free(text);
    return got;
}
