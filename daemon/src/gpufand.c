// gpufand — связывает датчик(и) температуры SMC с PWM-вентиляторами материнской платы.
//
// Работает поверх SMCSuperIO (Nuvoton NCT679x fan control), общаясь только через SMC-ключи:
//   F<n>Md = 2   ручной режим "duty": F<n>Tg — скважность в процентах 0..100 (точно, без обратной связи)
//   F<n>Md = 0   вернуть вентилятор BIOS SmartFan
// Поэтому демон не зависит от кекста напрямую и уживается со сторонними утилитами.
//
// Состояние для GUI (будущее приложение в строке меню): /var/run/gpufand.json, обновляется каждый цикл.
// SIGHUP — перечитать конфиг. SIGTERM/SIGINT — вернуть вентиляторы BIOS и выйти.
//
// Build: cc -O2 -Wall -o gpufand gpufand.c -framework IOKit
// Usage: gpufand [-c /usr/local/etc/gpufand.conf] [--dry-run] [--once] [--restore]

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include "smc_io.h"

#define MAX_FANS     8
#define MAX_SOURCES  4
#define MAX_KEYS     4
#define MAX_POINTS   8
#define STATUS_PATH  "/var/run/gpufand.json"
#define REFRESH_SEC  10     // re-write Md/Tg at least this often (another app may have changed them)

typedef struct { double t, duty; } CurvePoint;
typedef struct {
    char keys[MAX_KEYS][5];
    int nkeys;
    CurvePoint pts[MAX_POINTS];
    int npts;
    int fails;
    int hot;            // index of the key that was hottest last time it was read
    double last;        // last good temperature
    double temp;        // temperature used this cycle (NAN on failure)
} Source;

typedef struct {
    int fans[MAX_FANS];
    int nfans;
    Source src[MAX_SOURCES];
    int nsrc;
    double interval;        // s
    double min_duty;        // % floor while controlling
    double fail_duty;       // % when a source fails fail_limit cycles in a row
    double down_step;       // max % decrease per cycle
    double hysteresis;      // °C: duty goes down only when temp is this far below the curve point
    int fail_limit;
} Config;

static const Config defaults = {
    .interval = 2, .min_duty = 25, .fail_duty = 100, .down_step = 2, .hysteresis = 3, .fail_limit = 3,
};
static Config cfg;

static volatile sig_atomic_t g_stop = 0, g_reload = 0;
static int g_dry = 0;

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...) {
    char ts[32]; time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    va_list ap; va_start(ap, fmt);
    fprintf(stdout, "%s gpufand: ", ts); vfprintf(stdout, fmt, ap); fputc('\n', stdout);
    va_end(ap); fflush(stdout);
}

static void on_stop(int sig) { (void)sig; g_stop = 1; }
static void on_hup(int sig) { (void)sig; g_reload = 1; }

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

// source = KEY[,KEY...] : T:D T:D ...
static int parse_source(Config *c, char *v) {
    if (c->nsrc >= MAX_SOURCES) return -1;
    Source *s = &c->src[c->nsrc];
    memset(s, 0, sizeof(*s));
    char *colon = strchr(v, ':');
    if (!colon) return -1;
    *colon = 0;
    char *keys = trim(v), *curve = trim(colon + 1), *tok, *save;
    for (tok = strtok_r(keys, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = trim(tok);
        if (strlen(tok) != 4 || s->nkeys >= MAX_KEYS) return -1;
        memcpy(s->keys[s->nkeys++], tok, 5);
    }
    for (tok = strtok_r(curve, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        double t, d;
        if (sscanf(tok, "%lf:%lf", &t, &d) != 2 || s->npts >= MAX_POINTS) return -1;
        if (s->npts && t <= s->pts[s->npts - 1].t) return -1;
        if (d < 0 || d > 100) return -1;
        s->pts[s->npts++] = (CurvePoint){ t, d };
    }
    if (!s->nkeys || s->npts < 2) return -1;
    c->nsrc++;
    return 0;
}

static int load_config(const char *path, Config *c) {
    *c = defaults;
    FILE *f = fopen(path, "r");
    if (!f) { logmsg("cannot open config %s", path); return -1; }
    char line[512]; int ln = 0, err = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *l = trim(line);
        if (!*l) continue;
        char *eq = strchr(l, '=');
        if (!eq) { logmsg("config:%d: expected key=value", ln); err = 1; continue; }
        *eq = 0;
        char *k = trim(l), *v = trim(eq + 1);
        if (!strcmp(k, "fans")) {
            char *tok, *save;
            for (tok = strtok_r(v, ",", &save); tok && c->nfans < MAX_FANS; tok = strtok_r(NULL, ",", &save)) {
                int n = atoi(trim(tok));
                if (n < 0 || n > 15) { logmsg("config:%d: bad fan index", ln); err = 1; continue; }
                c->fans[c->nfans++] = n;
            }
        } else if (!strcmp(k, "source")) {
            if (parse_source(c, v)) { logmsg("config:%d: bad source (KEY[,KEY]: T:D T:D ..., ascending T, D 0..100)", ln); err = 1; }
        } else if (!strcmp(k, "interval")) c->interval = atof(v);
        else if (!strcmp(k, "min_duty")) c->min_duty = atof(v);
        else if (!strcmp(k, "fail_duty")) c->fail_duty = atof(v);
        else if (!strcmp(k, "down_step")) c->down_step = atof(v);
        else if (!strcmp(k, "hysteresis")) c->hysteresis = atof(v);
        else if (!strcmp(k, "fail_limit")) c->fail_limit = atoi(v);
        else { logmsg("config:%d: unknown key '%s'", ln, k); err = 1; }
    }
    fclose(f);
    if (c->interval < 0.5 || c->interval > 10) { logmsg("interval must be 0.5..10 s"); err = 1; }
    if (!c->nfans) { logmsg("config: no fans"); err = 1; }
    if (!c->nsrc) { logmsg("config: no source"); err = 1; }
    return err ? -1 : 0;
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static double curve(const Source *s, double t) {
    if (t <= s->pts[0].t) return s->pts[0].duty;
    for (int i = 1; i < s->npts; i++) {
        if (t <= s->pts[i].t) {
            const CurvePoint *a = &s->pts[i - 1], *b = &s->pts[i];
            return a->duty + (t - a->t) * (b->duty - a->duty) / (b->t - a->t);
        }
    }
    return s->pts[s->npts - 1].duty;
}

static void fankey(char out[5], int fan, const char *suffix) {
    static const char idx[] = "0123456789ABCDEF";
    out[0] = 'F'; out[1] = idx[fan & 15]; out[2] = suffix[0]; out[3] = suffix[1]; out[4] = 0;
}

// A source is the hottest of its keys; remember which one won so logs and status
// name the sensor the curve actually followed, not just the first key listed.
static int read_source(Source *s, double *temp) {
    int ok = 0, hot = 0; double best = -1000;
    for (int i = 0; i < s->nkeys; i++) {
        double v;
        if (smc_read_num(s->keys[i], &v) == 0 && v > 0 && v < 150) {
            ok = 1;
            if (v > best) { best = v; hot = i; }
        }
    }
    if (!ok) return -1;
    s->hot = hot;
    *temp = best;
    return 0;
}

static int set_fan(int fan, double duty) {
    if (g_dry) return 0;
    char kTg[5], kMd[5];
    fankey(kTg, fan, "Tg"); fankey(kMd, fan, "Md");
    int rc = smc_write_fpe2(kTg, clampd(duty, 0, 100));   // target first, then mode
    if (!rc) rc = smc_write_ui8(kMd, 2);
    if (rc) logmsg("fan %d: write %s/%s failed (%d)", fan, kTg, kMd, rc);
    return rc;
}

static void restore_all(const Config *c) {
    for (int i = 0; i < c->nfans; i++) {
        char kMd[5]; fankey(kMd, c->fans[i], "Md");
        int rc = smc_write_ui8(kMd, 0);
        if (rc) logmsg("fan %d: restore %s failed (%d)", c->fans[i], kMd, rc);
    }
    logmsg("fans returned to BIOS control");
}

// Fans must expose Md; every source must have at least one existing key.
static int validate(Config *c) {
    for (int i = 0; i < c->nfans; i++) {
        char k[5]; fankey(k, c->fans[i], "Md");
        if (smc_info(k, NULL, NULL)) {
            logmsg("fan %d: key %s missing — SMCSuperIO with fan control not loaded or wrong index", c->fans[i], k);
            return -1;
        }
    }
    for (int s = 0; s < c->nsrc; s++) {
        Source *src = &c->src[s];
        int n = 0;
        for (int k = 0; k < src->nkeys; k++) {
            if (smc_info(src->keys[k], NULL, NULL) == 0) memmove(src->keys[n++], src->keys[k], 5);
            else logmsg("source %d: key %s not present, ignored", s, src->keys[k]);
        }
        src->nkeys = n;
        if (!n) {
            // Never run a curve without its sensor (a passive GPU would cook): leave fans to BIOS.
            logmsg("source %d: none of its keys present — fix config", s);
            return -1;
        }
    }
    return 0;
}

static void write_status(const Config *c, double duty, int failed) {
    if (g_dry) return;
    char tmp[] = STATUS_PATH ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "{\"time\":%ld,\"duty\":%.1f,\"failed\":%s,\"sources\":[", (long)time(NULL), duty, failed ? "true" : "false");
    for (int s = 0; s < c->nsrc; s++) {
        const Source *src = &c->src[s];
        fprintf(f, "%s{\"keys\":[", s ? "," : "");
        for (int k = 0; k < src->nkeys; k++) fprintf(f, "%s\"%s\"", k ? "," : "", src->keys[k]);
        if (isnan(src->temp)) fprintf(f, "],\"hot\":null,\"temp\":null}");
        else fprintf(f, "],\"hot\":\"%s\",\"temp\":%.1f}", src->keys[src->hot], src->temp);
    }
    fprintf(f, "],\"fans\":[");
    for (int i = 0; i < c->nfans; i++) {
        char kAc[5]; double rpm = -1;
        fankey(kAc, c->fans[i], "Ac");
        smc_read_num(kAc, &rpm);
        fprintf(f, "%s{\"index\":%d,\"rpm\":%.0f}", i ? "," : "", c->fans[i], rpm);
    }
    fprintf(f, "]}\n");
    fclose(f);
    rename(tmp, STATUS_PATH);
}

int main(int argc, char **argv) {
    const char *conf = "/usr/local/etc/gpufand.conf";
    int once = 0, restore = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) conf = argv[++i];
        else if (!strcmp(argv[i], "--dry-run")) g_dry = 1;
        else if (!strcmp(argv[i], "--once")) { once = 1; g_dry = 1; }
        else if (!strcmp(argv[i], "--restore")) restore = 1;
        else { fprintf(stderr, "usage: %s [-c conf] [--dry-run] [--once] [--restore]\n", argv[0]); return 2; }
    }
    if (load_config(conf, &cfg)) return 2;
    if (smc_open()) { logmsg("cannot open AppleSMC"); return 1; }
    if (restore) { restore_all(&cfg); return 0; }
    if (validate(&cfg)) return 1;

    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);
    signal(SIGHUP, on_hup);

    double duty = -1, last_logged = -1, last_written = -1;
    time_t last_write_time = 0;
    logmsg("started: %d fans, %d sources, interval %.1fs%s", cfg.nfans, cfg.nsrc, cfg.interval, g_dry ? " (dry run)" : "");

    while (!g_stop) {
        if (g_reload) {
            g_reload = 0;
            Config next;
            if (load_config(conf, &next) == 0 && validate(&next) == 0) {
                // fans dropped from the new config go back to BIOS
                for (int i = 0; i < cfg.nfans; i++) {
                    int kept = 0;
                    for (int j = 0; j < next.nfans; j++) kept |= next.fans[j] == cfg.fans[i];
                    if (!kept && !g_dry) { char k[5]; fankey(k, cfg.fans[i], "Md"); smc_write_ui8(k, 0); }
                }
                cfg = next; last_written = -1;
                logmsg("config reloaded: %d fans, %d sources", cfg.nfans, cfg.nsrc);
            } else {
                logmsg("config reload failed, keeping previous config");
            }
        }

        double up = 0, down = 0; int failed = 0;
        char desc[256] = ""; size_t dl = 0;
        for (int s = 0; s < cfg.nsrc; s++) {
            Source *src = &cfg.src[s];
            double t, du, dd;
            if (read_source(src, &t) == 0) {
                src->fails = 0; src->last = t;
            } else if (++src->fails >= cfg.fail_limit || src->last <= 0) {
                failed = 1; t = NAN;
            } else {
                t = src->last;                          // transient glitch: hold last value
            }
            src->temp = t;
            if (isnan(t)) { du = dd = cfg.fail_duty; }
            else { du = curve(src, t); dd = curve(src, t + cfg.hysteresis); }
            if (du > up) up = du;
            if (dd > down) down = dd;
            if (dl < sizeof desc)
                dl += snprintf(desc + dl, sizeof desc - dl, "%s%s=%.1f", s ? " " : "", src->keys[src->hot], t);
        }
        up = clampd(up, cfg.min_duty, 100);
        down = clampd(down, cfg.min_duty, 100);

        if (duty < 0 || up > duty)
            duty = up;                                  // rise immediately
        else if (down < duty)                           // fall slowly, with hysteresis
            duty = down > duty - cfg.down_step ? down : duty - cfg.down_step;

        time_t now = time(NULL);
        if (fabs(duty - last_written) >= 0.5 || now - last_write_time >= REFRESH_SEC) {
            for (int i = 0; i < cfg.nfans; i++) set_fan(cfg.fans[i], duty);
            last_written = duty; last_write_time = now;
        }
        write_status(&cfg, duty, failed);

        if (once || fabs(duty - last_logged) >= 5 || failed) {
            logmsg("%s -> duty %.0f%%%s", desc, duty, failed ? " (SENSOR FAILURE)" : "");
            last_logged = duty;
        }
        if (once) return 0;

        struct timespec ts = { (time_t)cfg.interval, (long)((cfg.interval - (time_t)cfg.interval) * 1e9) };
        while (!g_stop && !g_reload && nanosleep(&ts, &ts) == -1) {}
    }

    if (!g_dry) { restore_all(&cfg); unlink(STATUS_PATH); }
    return 0;
}
