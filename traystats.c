/*
 * traystats - a small, dependency-light X11 system-tray monitor.
 *
 *   CPU slot   a spinning turbine whose speed follows the fan tachometer,
 *              ringed by a CPU-load gauge and tinted by temperature.
 *   RAM slot   a memory chip filled with a gently rippling liquid.
 *   Disk slot  a hard-drive platter with a read/write actuator arm that
 *              seeks across the tracks while the disks are busy.
 *
 * All artwork is drawn by a tiny software rasteriser (analytic anti-aliased
 * signed-distance shapes) and composited over the real tray background, so
 * it looks crisp at any icon size and needs nothing beyond Xlib and libm.
 */
#define _POSIX_C_SOURCE 200809L

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PI 3.14159265358979323846
#define TAU (2.0 * PI)

#define DEFAULT_ICON_SIZE 32
#define FRAME_SEC 0.040      /* animation frame interval            */
#define IDLE_FRAME_SEC 0.250 /* frame interval while nothing shows  */
#define SAMPLE_SEC 0.400     /* sensor sampling interval            */
#define RESCAN_SEC 30.0      /* re-discover hwmon devices           */
#define DOCK_RETRY_SEC 1.5
#define BG_REFRESH_SEC 30.0
#define BG_MIN_GAP_SEC 0.15
#define MAX_PATHS 48
#define PATH_LEN 112

#define MENU_W 208
#define MENU_PAD 8
#define MENU_ROW 32
#define MENU_SEP 12
#define MENU_H (MENU_PAD * 2 + MENU_ROW * 4 + MENU_SEP)

enum metric { METRIC_CPU, METRIC_RAM, METRIC_IO, METRIC_COUNT };
#define ROW_QUIT METRIC_COUNT

static volatile sig_atomic_t running = 1;
static volatile int x_error_seen = 0;
static int debug_enabled = 0;

static void on_signal(int number)
{
    (void)number;
    running = 0;
}

static int on_x_error(Display *display, XErrorEvent *event)
{
    x_error_seen = 1;
    if (debug_enabled) {
        char text[96];
        XGetErrorText(display, event->error_code, text, sizeof(text));
        fprintf(stderr, "traystats: X error: %s (request %d)\n", text,
                event->request_code);
    }
    return 0;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static double mixd(double a, double b, double t)
{
    return a + (b - a) * t;
}

static double smoothstep(double e0, double e1, double x)
{
    double t = clampd((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/* ====================================================================== */
/*  Sensors                                                               */
/* ====================================================================== */

typedef struct {
    char path[MAX_PATHS][PATH_LEN];
    int count;
} PathList;

typedef struct {
    PathList fans;
    PathList temps_cpu;   /* sensors that belong to the processor     */
    PathList temps_other; /* fallback: anything that reports a temp   */
    double scanned;
} Sensors;

typedef struct {
    int has_rpm, has_temp;
    double rpm, temp;
    double cpu_load;  /* 0..1 */
    double ram_used;  /* 0..1 */
    double ram_total_gib, ram_used_gib;
    double read_mbs, write_mbs;
    double io_busy;   /* 0..1 */
} Stats;

typedef struct {
    Sensors sensors;
    unsigned long long cpu_total, cpu_idle;
    int cpu_valid;
    unsigned long long sec_r, sec_w, io_ms;
    double io_time;
    int io_valid;
} Sampler;

static int read_line(const char *path, char *buf, size_t size)
{
    FILE *file = fopen(path, "r");

    if (!file)
        return -1;
    if (!fgets(buf, (int)size, file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static int read_number(const char *path, double *value)
{
    char text[64];
    char *end;

    if (read_line(path, text, sizeof(text)) != 0)
        return -1;
    *value = strtod(text, &end);
    return end == text ? -1 : 0;
}

static void path_add(PathList *list, const char *path)
{
    if (list->count >= MAX_PATHS || strlen(path) >= PATH_LEN)
        return;
    strcpy(list->path[list->count++], path);
}

static int is_cpu_chip(const char *name)
{
    static const char *const known[] = {"coretemp", "k10temp", "zenpower",
                                        "cpu_thermal", "x86_pkg_temp",
                                        "cpu"};
    size_t i;

    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (strstr(name, known[i]))
            return 1;
    return 0;
}

static int has_suffix(const char *text, const char *suffix)
{
    size_t a = strlen(text), b = strlen(suffix);

    return a >= b && strcmp(text + a - b, suffix) == 0;
}

static void sensors_scan(Sensors *sensors)
{
    DIR *hwmon;
    struct dirent *entry;

    memset(sensors, 0, sizeof(*sensors));
    sensors->scanned = now_sec();

    hwmon = opendir("/sys/class/hwmon");
    while (hwmon && (entry = readdir(hwmon)) != NULL) {
        char dir[96], file[160], name[64] = "";
        DIR *sub;
        struct dirent *item;
        int cpu_chip;

        if (strncmp(entry->d_name, "hwmon", 5) != 0)
            continue;
        if (snprintf(dir, sizeof(dir), "/sys/class/hwmon/%s",
                     entry->d_name) >= (int)sizeof(dir))
            continue;
        if (snprintf(file, sizeof(file), "%s/name", dir) < (int)sizeof(file))
            read_line(file, name, sizeof(name));
        cpu_chip = is_cpu_chip(name);
        sub = opendir(dir);
        if (!sub)
            continue;
        while ((item = readdir(sub)) != NULL) {
            if (!has_suffix(item->d_name, "_input"))
                continue;
            if (snprintf(file, sizeof(file), "%s/%s", dir, item->d_name) >=
                (int)sizeof(file))
                continue;
            if (strncmp(item->d_name, "fan", 3) == 0 &&
                isdigit((unsigned char)item->d_name[3]))
                path_add(&sensors->fans, file);
            else if (strncmp(item->d_name, "temp", 4) == 0 &&
                     isdigit((unsigned char)item->d_name[4]))
                path_add(cpu_chip ? &sensors->temps_cpu
                                  : &sensors->temps_other,
                         file);
        }
        closedir(sub);
    }
    if (hwmon)
        closedir(hwmon);

    /* Thermal zones are the fallback for machines without a hwmon CPU chip. */
    hwmon = opendir("/sys/class/thermal");
    while (hwmon && (entry = readdir(hwmon)) != NULL) {
        char file[160], type[64] = "";

        if (strncmp(entry->d_name, "thermal_zone", 12) != 0)
            continue;
        if (snprintf(file, sizeof(file), "/sys/class/thermal/%s/type",
                     entry->d_name) < (int)sizeof(file))
            read_line(file, type, sizeof(type));
        if (snprintf(file, sizeof(file), "/sys/class/thermal/%s/temp",
                     entry->d_name) >= (int)sizeof(file))
            continue;
        path_add(is_cpu_chip(type) ? &sensors->temps_cpu
                                   : &sensors->temps_other,
                 file);
    }
    if (hwmon)
        closedir(hwmon);
}

static int read_max(const PathList *list, double scale, double lo, double hi,
                    double *result)
{
    int found = 0, i;

    for (i = 0; i < list->count; i++) {
        double value;

        if (read_number(list->path[i], &value) != 0)
            continue;
        value /= scale;
        if (value < lo || value > hi)
            continue;
        if (!found || value > *result)
            *result = value;
        found = 1;
    }
    return found ? 0 : -1;
}

static double sample_cpu(Sampler *sampler)
{
    FILE *file = fopen("/proc/stat", "r");
    char line[256];
    unsigned long long v[8] = {0};
    double load = 0.0;

    if (!file)
        return 0.0;
    if (fgets(line, sizeof(line), file) &&
        sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0],
               &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
        unsigned long long total = 0, idle = v[3] + v[4];
        int i;

        for (i = 0; i < 8; i++)
            total += v[i];
        if (sampler->cpu_valid && total > sampler->cpu_total &&
            idle >= sampler->cpu_idle)
            load = 1.0 - (double)(idle - sampler->cpu_idle) /
                             (double)(total - sampler->cpu_total);
        sampler->cpu_total = total;
        sampler->cpu_idle = idle;
        sampler->cpu_valid = 1;
    }
    fclose(file);
    return clampd(load, 0.0, 1.0);
}

static void sample_memory(Stats *stats)
{
    FILE *file = fopen("/proc/meminfo", "r");
    char line[160];
    unsigned long long total = 0, avail = 0, free_kb = 0, buffers = 0,
                       cached = 0, value;

    if (!file)
        return;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "MemTotal: %llu", &value) == 1)
            total = value;
        else if (sscanf(line, "MemAvailable: %llu", &value) == 1)
            avail = value;
        else if (sscanf(line, "MemFree: %llu", &value) == 1)
            free_kb = value;
        else if (sscanf(line, "Buffers: %llu", &value) == 1)
            buffers = value;
        else if (sscanf(line, "Cached: %llu", &value) == 1)
            cached = value;
    }
    fclose(file);
    if (!avail)
        avail = free_kb + buffers + cached;
    if (avail > total)
        avail = total;
    if (total) {
        stats->ram_used = 1.0 - (double)avail / (double)total;
        stats->ram_total_gib = (double)total / 1048576.0;
        stats->ram_used_gib = (double)(total - avail) / 1048576.0;
    }
}

/*
 * /proc/diskstats lists partitions as well as whole disks, so summing every
 * line double-counts. Only devices that own a /sys/block entry are real
 * disks; stacked virtual devices would count the same traffic again.
 */
static int is_whole_disk(const char *device)
{
    static const char *const skip[] = {"loop", "ram", "zram", "dm-", "md",
                                       "sr",   "fd",  "nbd"};
    char path[96], name[48];
    size_t i;

    for (i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
        if (strncmp(device, skip[i], strlen(skip[i])) == 0)
            return 0;
    snprintf(name, sizeof(name), "%s", device);
    for (i = 0; name[i]; i++)
        if (name[i] == '/')
            name[i] = '!';
    snprintf(path, sizeof(path), "/sys/block/%s", name);
    return access(path, F_OK) == 0;
}

static void sample_io(Sampler *sampler, Stats *stats)
{
    FILE *file = fopen("/proc/diskstats", "r");
    char line[512];
    unsigned long long sec_r = 0, sec_w = 0, io_ms = 0;
    double now, dt;

    stats->read_mbs = stats->write_mbs = stats->io_busy = 0.0;
    if (!file)
        return;
    while (fgets(line, sizeof(line), file)) {
        unsigned major, minor;
        char device[48];
        unsigned long long rd, rm, rs, rt, wr, wm, ws, wt, inflight, ticks;

        if (sscanf(line,
                   "%u %u %47s %llu %llu %llu %llu %llu %llu %llu %llu %llu "
                   "%llu",
                   &major, &minor, device, &rd, &rm, &rs, &rt, &wr, &wm, &ws,
                   &wt, &inflight, &ticks) != 13)
            continue;
        if (!is_whole_disk(device))
            continue;
        sec_r += rs;
        sec_w += ws;
        io_ms += ticks;
    }
    fclose(file);
    now = now_sec();
    if (sampler->io_valid && now > sampler->io_time) {
        dt = now - sampler->io_time;
        stats->read_mbs = (sec_r >= sampler->sec_r ? sec_r - sampler->sec_r : 0)
                          * 512.0 / 1048576.0 / dt;
        stats->write_mbs = (sec_w >= sampler->sec_w ? sec_w - sampler->sec_w : 0)
                           * 512.0 / 1048576.0 / dt;
        stats->io_busy = clampd(
            (double)(io_ms >= sampler->io_ms ? io_ms - sampler->io_ms : 0) /
                (dt * 1000.0),
            0.0, 1.0);
    }
    sampler->sec_r = sec_r;
    sampler->sec_w = sec_w;
    sampler->io_ms = io_ms;
    sampler->io_time = now;
    sampler->io_valid = 1;
}

static void sample_stats(Sampler *sampler, Stats *stats)
{
    Sensors *s = &sampler->sensors;
    double value;

    if (now_sec() - s->scanned > RESCAN_SEC)
        sensors_scan(s);
    stats->has_rpm = read_max(&s->fans, 1.0, 0.0, 100000.0, &value) == 0;
    stats->rpm = stats->has_rpm ? value : 0.0;
    stats->has_temp =
        read_max(&s->temps_cpu, 1000.0, 1.0, 125.0, &value) == 0 ||
        read_max(&s->temps_other, 1000.0, 1.0, 125.0, &value) == 0;
    stats->temp = stats->has_temp ? value : 0.0;
    stats->cpu_load = sample_cpu(sampler);
    sample_memory(stats);
    sample_io(sampler, stats);
}

/* ====================================================================== */
/*  Animation state                                                       */
/* ====================================================================== */

#define ARM_INNER 0.27 /* arm angle (rad) when the head is near the spindle */
#define ARM_OUTER 0.68 /* ... and near the rim                              */
#define ARM_PARK 0.873 /* ... and parked clear of the platter               */

typedef struct {
    double t;
    int has_temp;
    double temp, load, ram, act, wr_mix;
    double fan_phase, fan_rps;
    double spin;
    double arm, arm_v, arm_target, seek_timer, idle_time;
    unsigned rng;
} Anim;

static double anim_random(Anim *an)
{
    an->rng ^= an->rng << 13;
    an->rng ^= an->rng >> 17;
    an->rng ^= an->rng << 5;
    return (double)(an->rng & 0xffffff) / 16777216.0;
}

static void anim_init(Anim *an)
{
    memset(an, 0, sizeof(*an));
    an->rng = 2463534242u;
    an->arm = an->arm_target = ARM_PARK;
}

static double approach(double current, double target, double dt, double tau)
{
    return current + (target - current) * (1.0 - exp(-dt / tau));
}

static void anim_step(Anim *an, const Stats *st, double dt)
{
    double target, mbs, act_target, w;
    int i;

    an->t += dt;
    an->has_temp = st->has_temp;
    if (st->has_temp)
        an->temp = an->temp > 0.0 ? approach(an->temp, st->temp, dt, 0.8)
                                  : st->temp;
    an->load = approach(an->load, st->cpu_load, dt, 0.35);
    an->ram = approach(an->ram, st->ram_used, dt, 0.8);

    /* Fan: follow the tachometer; without one, derive a believable speed. */
    if (st->has_rpm) {
        target = st->rpm > 1.0
                     ? 0.07 + 0.98 * sqrt(clampd(st->rpm / 6000.0, 0.0, 1.0))
                     : 0.0;
    } else {
        double tf = st->has_temp ? clampd((st->temp - 35.0) / 55.0, 0.0, 1.0)
                                 : 0.2;
        target = 0.08 + 0.50 * tf * tf + 0.40 * an->load;
    }
    an->fan_rps = approach(an->fan_rps, fmin(target, 1.05), dt, 0.7);
    an->fan_phase = fmod(an->fan_phase + an->fan_rps * TAU * dt, TAU);

    /* Disk activity: fast attack, slow decay, log-scaled throughput. */
    mbs = st->read_mbs + st->write_mbs;
    act_target = clampd(fmax(st->io_busy, log10(1.0 + mbs) / 2.2), 0.0, 1.0);
    an->act = approach(an->act, act_target, dt,
                       act_target > an->act ? 0.08 : 0.7);
    if (mbs > 0.05)
        an->wr_mix = approach(an->wr_mix, st->write_mbs / mbs, dt, 0.3);
    an->spin = fmod(an->spin + (0.18 + 0.70 * an->act) * TAU * dt, TAU);

    /* Actuator arm: hop between random tracks while busy, park when idle. */
    if (an->act > 0.03) {
        an->idle_time = 0.0;
        an->seek_timer -= dt;
        if (an->seek_timer <= 0.0) {
            an->arm_target =
                ARM_INNER + (ARM_OUTER - ARM_INNER) * anim_random(an);
            an->seek_timer =
                (0.05 + 0.45 * (1.0 - an->act)) * (0.4 + anim_random(an));
        }
    } else {
        an->idle_time += dt;
        if (an->idle_time > 1.0)
            an->arm_target = ARM_PARK;
    }
    w = an->arm_target >= ARM_PARK ? 9.0 : 24.0;
    for (i = 0; i < 4; i++) {
        double h = dt / 4.0;
        double acc = w * w * (an->arm_target - an->arm) - 1.5 * w * an->arm_v;

        an->arm_v += acc * h;
        an->arm += an->arm_v * h;
    }
    an->arm = clampd(an->arm, ARM_INNER - 0.04, ARM_PARK + 0.02);
}

/* ====================================================================== */
/*  Software rasteriser: colours, canvas, signed-distance helpers         */
/* ====================================================================== */

typedef struct { double r, g, b; } Rgb;
typedef struct { double r, g, b, a; } Acc; /* premultiplied */

typedef struct {
    int w, h;
    float *px; /* premultiplied RGBA */
} Canvas;

typedef struct { double k, ox, oy; } View;

static Rgb hex(unsigned v)
{
    Rgb c = {((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0,
             (v & 255) / 255.0};
    return c;
}

static Rgb mixc(Rgb a, Rgb b, double t)
{
    Rgb c = {mixd(a.r, b.r, t), mixd(a.g, b.g, t), mixd(a.b, b.b, t)};
    return c;
}

static Rgb shade(Rgb c, double k)
{
    Rgb o = {c.r * k, c.g * k, c.b * k};
    return o;
}

static const Rgb WHITE = {1.0, 1.0, 1.0};
static const Rgb BLACK = {0.0, 0.0, 0.0};

static void over(Acc *acc, Rgb c, double a)
{
    if (a <= 0.0)
        return;
    if (a > 1.0)
        a = 1.0;
    acc->r = clampd(c.r, 0.0, 1.0) * a + acc->r * (1.0 - a);
    acc->g = clampd(c.g, 0.0, 1.0) * a + acc->g * (1.0 - a);
    acc->b = clampd(c.b, 0.0, 1.0) * a + acc->b * (1.0 - a);
    acc->a = a + acc->a * (1.0 - a);
}

/* Pixel coverage for a signed distance measured in pixels. */
static double cov(double d)
{
    return d <= -0.5 ? 1.0 : (d >= 0.5 ? 0.0 : 0.5 - d);
}

static Rgb ramp3(double x, double a0, double a1, double b0, double b1)
{
    Rgb green = hex(0x37e08a), amber = hex(0xffb938), red = hex(0xff4b4b);

    if (x <= a0)
        return green;
    if (x < a1)
        return mixc(green, amber, smoothstep(a0, a1, x));
    if (x <= b0)
        return amber;
    if (x < b1)
        return mixc(amber, red, smoothstep(b0, b1, x));
    return red;
}

/* green < 50 C, amber 50-70 C, red above 70 C, with soft transitions. */
static Rgb temp_color(double t)
{
    return ramp3(t, 46.0, 54.0, 66.0, 74.0);
}

static Rgb usage_color(double u)
{
    return ramp3(u, 0.55, 0.68, 0.80, 0.92);
}

static double sd_rrect(double x, double y, double hw, double hh, double r)
{
    double qx = fabs(x) - hw + r, qy = fabs(y) - hh + r;

    return hypot(fmax(qx, 0.0), fmax(qy, 0.0)) + fmin(fmax(qx, qy), 0.0) - r;
}

typedef struct { double ax, ay, bx, by, ra, rb; } Seg;

/* Tapered capsule; optionally returns position along it and lateral offset. */
static double sd_tapered(const Seg *s, double x, double y, double *along,
                         double *lateral)
{
    double pax = x - s->ax, pay = y - s->ay;
    double bax = s->bx - s->ax, bay = s->by - s->ay;
    double bb = bax * bax + bay * bay;
    double t = clampd((pax * bax + pay * bay) / bb, 0.0, 1.0);
    double qx = pax - bax * t, qy = pay - bay * t;
    double rad = mixd(s->ra, s->rb, t);

    if (along)
        *along = t;
    if (lateral)
        *lateral = (bax * pay - bay * pax) / sqrt(bb) / rad;
    return sqrt(qx * qx + qy * qy) - rad;
}

static int canvas_resize(Canvas *cv, int w, int h)
{
    float *px;

    if (cv->w == w && cv->h == h && cv->px)
        return 0;
    px = realloc(cv->px, (size_t)w * (size_t)h * 4 * sizeof(float));
    if (!px)
        return -1;
    cv->px = px;
    cv->w = w;
    cv->h = h;
    return 0;
}

static View view_for(const Canvas *cv)
{
    double side = cv->w < cv->h ? cv->w : cv->h;
    View v = {side / 32.0, (cv->w - side) / 2.0, (cv->h - side) / 2.0};

    return v;
}

static void store(Canvas *cv, int x, int y, const Acc *a)
{
    float *p = cv->px + ((size_t)y * (size_t)cv->w + (size_t)x) * 4;

    p[0] = (float)a->r;
    p[1] = (float)a->g;
    p[2] = (float)a->b;
    p[3] = (float)a->a;
}

static void blit(Canvas *dst, const Canvas *src, int ox, int oy, double opacity)
{
    int x, y;

    for (y = 0; y < src->h; y++) {
        for (x = 0; x < src->w; x++) {
            const float *s = src->px + ((size_t)y * (size_t)src->w + (size_t)x) * 4;
            int dx = ox + x, dy = oy + y;
            float *d;
            double a = s[3] * opacity, inv;

            if (dx < 0 || dy < 0 || dx >= dst->w || dy >= dst->h)
                continue;
            d = dst->px + ((size_t)dy * (size_t)dst->w + (size_t)dx) * 4;
            inv = 1.0 - a;
            d[0] = (float)(s[0] * opacity + d[0] * inv);
            d[1] = (float)(s[1] * opacity + d[1] * inv);
            d[2] = (float)(s[2] * opacity + d[2] * inv);
            d[3] = (float)(a + d[3] * inv);
        }
    }
}

/* ====================================================================== */
/*  Icon: CPU / fan                                                       */
/* ====================================================================== */

static void render_cpu(Canvas *cv, const Anim *an, const Stats *st)
{
    View v = view_for(cv);
    double k = v.k;
    int blades = (cv->w < 36 && cv->h < 36) ? 5 : 7;
    double sector = TAU / blades;
    double sweep = 0.55, blade_hw = blades == 7 ? 2.5 : 2.9;
    const double r0 = 2.4, r1 = 10.6, gauge_r = 13.0, disc_r = 14.6;
    Rgb heat = an->has_temp ? temp_color(an->temp) : hex(0x6fb4e8);
    double hot = an->has_temp ? clampd((an->temp - 45.0) / 45.0, 0.0, 1.0)
                              : 0.0;
    double blur = clampd((an->fan_rps - 0.45) / 0.6, 0.0, 1.0);
    double load = clampd(an->load, 0.0, 1.0);
    double arc_end = fmax(load, 0.012) * TAU;
    double pulse = 0.75 + 0.25 * sin(an->t * 3.0);
    int alarm = an->has_temp && an->temp >= 70.0;
    int badge_on = 0;
    int x, y;

    if (alarm) {
        double period = an->temp > 90.0 ? 0.18 : 0.75;

        badge_on = fmod(an->t, period * 2.0) < period;
    }
    (void)st;

    for (y = 0; y < cv->h; y++) {
        for (x = 0; x < cv->w; x++) {
            double ux = (x + 0.5 - v.ox) / k, uy = (y + 0.5 - v.oy) / k;
            double dx = ux - 16.0, dy = uy - 16.0;
            double r = sqrt(dx * dx + dy * dy), th = atan2(dy, dx);
            double d_disc = r - disc_r;
            double lightfac = 0.5 + 0.5 * cos(th + 2.356);
            Acc A = {0, 0, 0, 0};

            /* soft heat halo just outside the glass */
            if (d_disc > -0.6)
                over(&A, heat,
                     (0.10 + 0.34 * hot) * exp(-fmax(d_disc, 0.0) * 1.2) *
                         (1.0 - smoothstep(14.6, 16.2, r)));

            /* glass medallion */
            over(&A, mixc(hex(0x232b37), hex(0x0a0d13), clampd(r / disc_r, 0, 1)),
                 0.74 * cov(d_disc * k));
            over(&A, WHITE,
                 0.11 * cov(d_disc * k) *
                     pow(clampd(-(dx + dy) / 15.0, 0.0, 1.0), 2.0));
            over(&A, WHITE,
                 (0.08 + 0.26 * lightfac) *
                     cov((fabs(d_disc + 0.35) - 0.35) * k));

            /* CPU-load gauge */
            {
                double ring = fabs(r - gauge_r) - 0.72;
                double phi = fmod(th + PI / 2.0 + TAU, TAU);
                double arc;

                over(&A, WHITE, 0.13 * cov(ring * k));
                if (phi <= arc_end) {
                    arc = ring;
                } else {
                    double ex = gauge_r * sin(arc_end),
                           ey = -gauge_r * cos(arc_end);
                    double d_end = hypot(dx - ex, dy - ey);
                    double d_start = hypot(dx, dy + gauge_r);

                    arc = fmin(d_end, d_start) - 0.72;
                }
                if (arc < 1.0) {
                    double along = clampd(phi / arc_end, 0.0, 1.0);

                    over(&A, shade(heat, 0.62 + 0.55 * along),
                         cov(arc * k));
                    over(&A, heat, 0.18 * exp(-fmax(arc, 0.0) * 1.4) * hot);
                }
            }

            /* motion-blur veil that fades in as the fan speeds up */
            if (r < r1 + 0.6 && r > r0)
                over(&A, heat,
                     blur * 0.20 * (1.0 - smoothstep(r0, r1 + 0.6, r)));

            /* blades */
            if (r > 2.0 && r < r1 + 3.0) {
                double a = th - an->fan_phase;
                double f = a - sector * floor(a / sector + 0.5);
                double t = clampd((r - r0) / (r1 - r0), 0.0, 1.0);
                double tt = fmax(t, 0.001);
                double sw = -sweep * pow(tt, 1.25);
                double dsw = -sweep * 1.25 * pow(tt, 0.25) / (r1 - r0);
                double dl = (f - sw) * r;
                double dperp = dl / sqrt(1.0 + r * dsw * r * dsw);
                double hw = blade_hw * (0.40 + 0.60 * pow(tt, 0.6));
                double rnd = sqrt(clampd(1.0 - pow(t, 6.0), 0.0, 1.0));
                double dB = fmax(fabs(dperp) - hw * rnd, r - r1);

                over(&A, heat,
                     (0.06 + 0.20 * hot) * exp(-fmax(dB, 0.0) * 0.8));
                if (dB < 1.0) {
                    double side = clampd(dperp / fmax(hw * rnd, 0.2), -1, 1);
                    Rgb bc = mixc(shade(heat, 0.36), shade(heat, 1.0),
                                  smoothstep(0.0, 0.85, t));

                    bc = shade(bc, 0.78 + 0.30 * cos(th + 2.356));
                    bc = mixc(bc, WHITE, 0.55 * smoothstep(0.55, 1.0, side));
                    bc = shade(bc, 1.0 - 0.30 * smoothstep(-0.2, -1.0, side));
                    bc = mixc(bc, WHITE, 0.10 * exp(-side * side * 6.0));
                    over(&A, bc, cov(dB * k));
                }
            }

            /* hub */
            if (r < 4.8) {
                double lit = clampd(1.0 - hypot(dx + 1.1, dy + 1.3) / 4.8, 0, 1);
                Rgb metal = mixc(hex(0x232a34), hex(0xc3cdda), pow(lit, 1.4));

                over(&A, BLACK, 0.50 * cov((r - 3.9) * k));
                over(&A, metal, cov((r - 3.4) * k));
                over(&A, hex(0x0a0d12), 0.93 * cov((r - 1.9) * k));
                over(&A, heat, pulse * cov((r - 0.85) * k));
                over(&A, heat,
                     0.30 * pulse * (1.0 - smoothstep(0.85, 1.9, r)) *
                         (r > 0.85 ? 1.0 : 0.0));
                over(&A, WHITE,
                     0.65 * cov((hypot(dx + 1.5, dy + 1.7) - 0.55) * k));
            }

            /* thermostat alert badge */
            if (alarm && badge_on) {
                double bx = ux - 25.4, by = uy - 6.6;
                double br = sqrt(bx * bx + by * by);

                if (br < 8.0) {
                    Seg tube = {25.4, 3.9, 25.4, 7.8, 0.95, 0.95};
                    Seg merc = {25.4, 6.4, 25.4, 8.0, 0.45, 0.45};
                    Rgb red = hex(an->temp > 90.0 ? 0xff2b2b : 0xff4b4b);

                    over(&A, red, 0.40 * exp(-fmax(br - 5.0, 0.0) * 0.7));
                    over(&A, hex(0x2a0a0a), 0.96 * cov((br - 5.0) * k));
                    over(&A, red, cov((fabs(br - 4.5) - 0.45) * k));
                    over(&A, hex(0xf2f5f9),
                         cov(sd_tapered(&tube, ux, uy, NULL, NULL) * k));
                    over(&A, hex(0xf2f5f9),
                         cov((hypot(bx, by - 2.0) - 1.6) * k));
                    over(&A, red, cov(sd_tapered(&merc, ux, uy, NULL, NULL) * k));
                    over(&A, red, cov((hypot(bx, by - 2.0) - 1.05) * k));
                }
            }
            store(cv, x, y, &A);
        }
    }
}

/* ====================================================================== */
/*  Icon: memory chip with liquid level                                   */
/* ====================================================================== */

static void render_ram(Canvas *cv, const Anim *an)
{
    View v = view_for(cv);
    double k = v.k;
    double u = clampd(an->ram, 0.0, 1.0);
    Rgb liq = usage_color(u);
    double level = 22.5 - 13.0 * u;
    double amp = 0.55 * fmin(1.0, fmin(u * 10.0, (1.0 - u) * 10.0 + 0.2));
    int x, y;

    for (y = 0; y < cv->h; y++) {
        for (x = 0; x < cv->w; x++) {
            double ux = (x + 0.5 - v.ox) / k, uy = (y + 0.5 - v.oy) / k;
            double dx = ux - 16.0, dy = uy - 16.0, ax = fabs(dx);
            double d_body = sd_rrect(dx, dy, 9.4, 9.4, 2.8);
            double d_cav;
            Acc A = {0, 0, 0, 0};

            over(&A, BLACK,
                 0.40 * exp(-fmax(sd_rrect(dx - 0.4, dy - 0.9, 9.4, 9.4, 2.8),
                                  0.0) / 1.3));
            over(&A, liq,
                 0.34 * u * u * u * exp(-fmax(d_body, 0.0) * 0.9) *
                     (1.0 - smoothstep(12.0, 16.0, fmax(ax, fabs(dy)))));

            /* gold pins */
            if (ax > 7.5 && ax < 13.2) {
                double qy = dy / 4.4 + 1.5;
                double idx = clampd(floor(qy + 0.5), 0.0, 3.0);
                double yc = (idx - 1.5) * 4.4;
                double dp = sd_rrect(ax - 10.9, dy - yc, 2.0, 0.95, 0.35);
                Rgb gold = mixc(hex(0xf6dc8a), hex(0x8f6d28),
                                clampd((ax - 8.9) / 4.0 + (dy - yc) * 0.25,
                                       0.0, 1.0));

                over(&A, BLACK, 0.35 * cov((dp - 0.35) * k));
                over(&A, gold, cov(dp * k));
            }

            /* package */
            over(&A,
                 mixc(hex(0x232b36), hex(0x0e1218),
                      clampd(0.5 + (dx + dy) / 26.0, 0.0, 1.0)),
                 0.96 * cov(d_body * k));
            over(&A,
                 mixc(hex(0xaab4c2), hex(0x4a5360),
                      clampd(0.5 + (dx + dy) / 18.0, 0.0, 1.0)),
                 0.92 * cov((fabs(d_body + 0.5) - 0.5) * k));
            over(&A, WHITE, 0.06 * cov((fabs(d_body + 1.4) - 0.3) * k));

            /* liquid window */
            d_cav = sd_rrect(dx, dy, 6.5, 6.5, 1.5);
            if (d_cav < 1.0) {
                double inside = cov(d_cav * k);
                double s1 = level + amp * sin(ux * 0.95 - an->t * 2.4);
                double s2 = level + 0.35 + amp * 0.8 * sin(ux * 0.75 +
                                                           an->t * 1.7 + 1.6);
                int b;

                over(&A, hex(0x06080c), 0.92 * inside);
                if (u > 0.005) {
                    double depth = clampd((uy - s1) / (22.5 - s1 + 0.01), 0, 1);
                    Rgb col = mixc(shade(liq, 1.18), shade(liq, 0.50),
                                   pow(depth, 0.8));

                    over(&A, shade(liq, 0.62), 0.55 * inside * cov((s2 - uy) * k));
                    over(&A, col, inside * cov((s1 - uy) * k));
                    over(&A, WHITE,
                         0.55 * inside * cov((fabs(s1 - uy) - 0.28) * k));
                    if (u > 0.12) {
                        for (b = 0; b < 3; b++) {
                            double bx = 16.0 + (b - 1) * 3.4 +
                                        0.6 * sin(an->t * 1.3 + b * 2.0);
                            double ph = fmod(an->t * (0.16 + 0.05 * b) + b * 0.37,
                                             1.0);
                            double by = 22.0 - ph * (22.0 - level - 0.8);
                            double br = 0.45 + 0.15 * b;
                            double fade = 1.0 - ph * ph * ph;
                            double d = hypot(ux - bx, uy - by) - br;

                            if (by > s1 + 0.6) {
                                over(&A, WHITE,
                                     0.50 * fade * cov((fabs(d) - 0.15) * k));
                                over(&A, WHITE, 0.12 * fade * cov(d * k));
                            }
                        }
                    }
                }
                over(&A, WHITE,
                     0.10 * inside * smoothstep(-1.5, -9.0, dx + dy * 0.8));
                over(&A, WHITE, 0.16 * cov((fabs(d_cav + 0.3) - 0.3) * k));
            }
            over(&A, WHITE, 0.55 * cov((hypot(dx + 7.95, dy + 7.95) - 0.45) * k));
            store(cv, x, y, &A);
        }
    }
}

/* ====================================================================== */
/*  Icon: hard disk with actuator arm                                     */
/* ====================================================================== */

static void render_io(Canvas *cv, const Anim *an)
{
    View v = view_for(cv);
    double k = v.k;
    const double cx = 13.4, cy = 16.6, plat_r = 12.2;
    const double px = 27.3, py = 26.6, arm_len = 15.8;
    double phi = atan2(cy - py, cx - px) + an->arm;
    double cs = cos(phi), sn = sin(phi);
    double tipx = px + arm_len * cs, tipy = py + arm_len * sn;
    Seg arm = {px, py, tipx, tipy, 1.65, 0.72};
    Seg tail = {px, py, px - 4.4 * cs, py - 4.4 * sn, 1.5, 2.1};
    double act = an->act;
    Rgb ac = mixc(hex(0x4fd0ff), hex(0xffae3d), an->wr_mix);
    double head_r = hypot(tipx - cx, tipy - cy);
    int x, y;

    for (y = 0; y < cv->h; y++) {
        for (x = 0; x < cv->w; x++) {
            double ux = (x + 0.5 - v.ox) / k, uy = (y + 0.5 - v.oy) / k;
            double dx = ux - cx, dy = uy - cy;
            double r = sqrt(dx * dx + dy * dy), th = atan2(dy, dx);
            double d_plat = r - plat_r;
            double lightfac = 0.5 + 0.5 * cos(th + 2.356);
            double along, lat, d_arm, d_tail, d_pivot;
            Acc A = {0, 0, 0, 0};

            /* platter drop shadow, rim and face */
            over(&A, BLACK,
                 0.42 * exp(-fmax(hypot(dx - 0.5, dy - 0.9) - plat_r, 0.0) / 1.4));
            over(&A, mixc(hex(0x48505c), hex(0xd2dae4), lightfac),
                 cov(d_plat * k));
            if (r < plat_r - 0.9) {
                double rn = clampd(r / (plat_r - 0.9), 0.0, 1.0);
                double tr = (r - 4.8) / 1.15;
                double ring = fabs(tr - floor(tr + 0.5)) * 1.15;
                double lobe = pow(fmax(0.0, cos(2.0 * (th - an->spin))), 10.0);
                double lobe2 = pow(fmax(0.0, cos(2.0 * (th - an->spin) - 1.3)),
                                   28.0);
                double band = smoothstep(3.6, 5.0, r) *
                              (1.0 - smoothstep(10.2, 11.3, r));

                over(&A, mixc(hex(0x2a323e), hex(0x0f131a), pow(rn, 0.8)),
                     cov((r - (plat_r - 0.9)) * k));
                if (r > 4.6 && r < 11.0)
                    over(&A, WHITE, 0.09 * cov((ring - 0.07) * k));
                over(&A, hex(0xbcd2ff), (0.20 * lobe + 0.14 * lobe2) * band);
                /* glow where the head is reading or writing */
                over(&A, ac,
                     act * (0.55 * cov((fabs(r - head_r) - 0.40) * k) +
                            0.22 * exp(-(r - head_r) * (r - head_r) * 0.9)) *
                         (r < plat_r - 0.9 ? 1.0 : 0.0));
            }

            /* spindle hub */
            if (r < 4.6) {
                double lit = clampd(1.0 - hypot(dx + 1.0, dy + 1.2) / 4.4, 0, 1);

                over(&A, BLACK, 0.45 * cov((r - 3.9) * k));
                over(&A, mixc(hex(0x1e252e), hex(0xc9d3e0), pow(lit, 1.3)),
                     cov((r - 3.4) * k));
                over(&A, hex(0x0b0e13), 0.95 * cov((r - 1.25) * k));
                over(&A, WHITE,
                     0.60 * cov((hypot(dx + 1.4, dy + 1.6) - 0.5) * k));
            }

            /* arm shadow on the platter */
            {
                double sd = sd_tapered(&arm, ux - 0.8, uy - 1.0, NULL, NULL);

                over(&A, BLACK, 0.34 * smoothstep(1.0, -0.5, sd));
            }

            /* mounting block, counterweight coil */
            d_pivot = hypot(ux - px, uy - py);
            over(&A, hex(0x10141a), 0.80 * cov((d_pivot - 3.5) * k));
            over(&A, WHITE, 0.12 * cov((fabs(d_pivot - 3.3) - 0.25) * k));
            d_tail = sd_tapered(&tail, ux, uy, &along, &lat);
            over(&A, mixc(hex(0x6c7683), hex(0x3a424d), along) , cov(d_tail * k));
            over(&A, hex(0xd7a15a),
                 0.55 * cov(d_tail * k) *
                     smoothstep(0.55, 0.85, 1.0 - fabs(lat)) *
                     (0.5 + 0.5 * sin(along * 26.0)));

            /* actuator arm: brushed metal with a lit ridge */
            d_arm = sd_tapered(&arm, ux, uy, &along, &lat);
            if (d_arm < 1.0) {
                Rgb m = mixc(hex(0xe0e7f0), hex(0x8a95a4), along);

                m = mixc(m, WHITE, 0.45 * exp(-pow((lat + 0.35) / 0.35, 2.0)));
                m = shade(m, 1.0 - 0.38 * smoothstep(0.75, 1.0, fabs(lat)));
                over(&A, BLACK, 0.45 * cov((d_arm - 0.30) * k));
                over(&A, m, cov(d_arm * k));
            }

            /* pivot bearing */
            {
                double lit = clampd(1.0 - hypot(ux - px + 0.7, uy - py + 0.8) / 3.2,
                                    0.0, 1.0);

                over(&A, mixc(hex(0x4b5563), hex(0xdbe3ee), pow(lit, 1.2)),
                     cov((d_pivot - 2.3) * k));
                over(&A, hex(0x0d1015), 0.92 * cov((d_pivot - 0.95) * k));
                over(&A, WHITE,
                     0.50 * cov((hypot(ux - px + 0.35, uy - py + 0.4) - 0.28) * k));
            }

            /* read/write head */
            {
                double dh = hypot(ux - tipx, uy - tipy);

                over(&A, ac, act * 0.55 * exp(-dh * 0.75));
                over(&A, hex(0x1b2027), cov((dh - 1.15) * k));
                over(&A, mixc(hex(0x8a93a0), ac, clampd(act * 1.6, 0.0, 1.0)),
                     cov((dh - 0.62) * k));
                over(&A, WHITE, act * 0.9 * cov((dh - 0.28) * k));
            }
            store(cv, x, y, &A);
        }
    }
}

static void render_metric(Canvas *cv, int metric, const Anim *an,
                          const Stats *st)
{
    if (metric == METRIC_CPU)
        render_cpu(cv, an, st);
    else if (metric == METRIC_RAM)
        render_ram(cv, an);
    else
        render_io(cv, an);
}

/* ====================================================================== */
/*  X11 plumbing                                                          */
/* ====================================================================== */

typedef struct {
    unsigned long rmask, gmask, bmask;
    int rshift, gshift, bshift, rbits, gbits, bbits;
} PixFmt;

typedef struct {
    Window win;
    GC gc;
    int w, h;
    int active; /* window exists */
    int docked, mapped;
    double dock_time;
    Canvas cv;
    XImage *img;
    unsigned char *bg; /* RGB snapshot of the tray behind the icon */
    int bg_ok, bg_dirty;
    double bg_time;
    char tip[160];
} Slot;

typedef struct {
    Window win;
    GC gc;
    XFontStruct *font;
    int open, hover;
    Canvas cv;
    XImage *img;
    unsigned long fg, dim, quit;
} Menu;

typedef struct {
    Display *dpy;
    int screen;
    Window root;
    Visual *visual;
    int depth;
    PixFmt fmt;
    Atom tray_atom, manager, opcode, xembed_info, net_wm_name, utf8,
        net_wm_pid, kde_tray_for;
    Slot slots[METRIC_COUNT];
    int enabled[METRIC_COUNT];
    int icon_size;
    Menu menu;
    Sampler sampler;
    Stats stats;
    Anim anim;
    char tips[METRIC_COUNT][160];
    double last_tray_poll;
} App;

static const char *const metric_label[METRIC_COUNT] = {"CPU / fan", "Memory",
                                                       "Disk I/O"};

static void pixfmt_init(PixFmt *f, const Visual *vis)
{
    unsigned long m;

    f->rmask = vis->red_mask;
    f->gmask = vis->green_mask;
    f->bmask = vis->blue_mask;
#define INIT_CHANNEL(mask, shift, bits)                                       \
    do {                                                                      \
        m = (mask);                                                           \
        (shift) = 0;                                                          \
        (bits) = 0;                                                           \
        while (m && !(m & 1UL)) {                                             \
            m >>= 1;                                                          \
            (shift)++;                                                        \
        }                                                                     \
        while (m & 1UL) {                                                     \
            m >>= 1;                                                          \
            (bits)++;                                                         \
        }                                                                     \
    } while (0)
    INIT_CHANNEL(f->rmask, f->rshift, f->rbits);
    INIT_CHANNEL(f->gmask, f->gshift, f->gbits);
    INIT_CHANNEL(f->bmask, f->bshift, f->bbits);
#undef INIT_CHANNEL
}

static unsigned long pack_rgb(const PixFmt *f, int r, int g, int b)
{
    unsigned long rm = (1UL << f->rbits) - 1, gm = (1UL << f->gbits) - 1,
                  bm = (1UL << f->bbits) - 1;

    return (((unsigned long)r * rm + 127) / 255) << f->rshift |
           (((unsigned long)g * gm + 127) / 255) << f->gshift |
           (((unsigned long)b * bm + 127) / 255) << f->bshift;
}

static void unpack_rgb(const PixFmt *f, unsigned long p, unsigned char *out)
{
    unsigned long rm = (1UL << f->rbits) - 1, gm = (1UL << f->gbits) - 1,
                  bm = (1UL << f->bbits) - 1;

    out[0] = (unsigned char)(((p >> f->rshift) & rm) * 255 / rm);
    out[1] = (unsigned char)(((p >> f->gshift) & gm) * 255 / gm);
    out[2] = (unsigned char)(((p >> f->bshift) & bm) * 255 / bm);
}

static XImage *image_create(App *app, int w, int h)
{
    XImage *img = XCreateImage(app->dpy, app->visual, (unsigned)app->depth,
                               ZPixmap, 0, NULL, (unsigned)w, (unsigned)h, 32, 0);

    if (!img)
        return NULL;
    img->data = calloc((size_t)img->bytes_per_line, (size_t)h);
    if (!img->data) {
        XFree(img);
        return NULL;
    }
    return img;
}

/* Blend a premultiplied canvas over an optional RGB backdrop into an XImage. */
static void canvas_to_image(const App *app, const Canvas *cv,
                            const unsigned char *bg, XImage *img)
{
    int x, y;

    for (y = 0; y < cv->h; y++) {
        for (x = 0; x < cv->w; x++) {
            const float *p = cv->px + ((size_t)y * (size_t)cv->w + (size_t)x) * 4;
            double inv = 1.0 - p[3];
            double c[3];
            int i;

            for (i = 0; i < 3; i++) {
                double back = bg ? bg[((size_t)y * (size_t)cv->w + (size_t)x) * 3 +
                                      (size_t)i] : 0.0;

                c[i] = p[i] * 255.0 + back * inv;
            }
            XPutPixel(img, x, y,
                      pack_rgb(&app->fmt, (int)clampd(c[0] + 0.5, 0, 255),
                               (int)clampd(c[1] + 0.5, 0, 255),
                               (int)clampd(c[2] + 0.5, 0, 255)));
        }
    }
}

static void set_tip(App *app, Slot *slot, const char *text)
{
    if (!slot->active || strcmp(slot->tip, text) == 0)
        return;
    snprintf(slot->tip, sizeof(slot->tip), "%s", text);
    XStoreName(app->dpy, slot->win, text);
    XChangeProperty(app->dpy, slot->win, app->net_wm_name, app->utf8, 8,
                    PropModeReplace, (const unsigned char *)text,
                    (int)strlen(text));
}

static int dock_request(App *app, Slot *slot)
{
    XClientMessageEvent ev;
    Window owner = XGetSelectionOwner(app->dpy, app->tray_atom);

    if (owner == None)
        return -1;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.window = owner;
    ev.message_type = app->opcode;
    ev.format = 32;
    ev.data.l[0] = CurrentTime;
    ev.data.l[1] = 0; /* SYSTEM_TRAY_REQUEST_DOCK */
    ev.data.l[2] = (long)slot->win;
    XSendEvent(app->dpy, owner, False, NoEventMask, (XEvent *)&ev);
    XSync(app->dpy, False);
    slot->dock_time = now_sec();
    return 0;
}

static void slot_create(App *app, int metric)
{
    Slot *slot = &app->slots[metric];
    XSetWindowAttributes attr;
    XSizeHints hints;
    XClassHint klass = {"traystats", "Traystats"};
    unsigned long embed[2] = {0, 1}; /* version 0, XEMBED_MAPPED */
    unsigned long pid = (unsigned long)getpid();

    memset(slot, 0, sizeof(*slot));
    attr.background_pixmap = ParentRelative;
    attr.event_mask = ExposureMask | StructureNotifyMask | ButtonPressMask;
    slot->win = XCreateWindow(app->dpy, app->root, 0, 0,
                              (unsigned)app->icon_size,
                              (unsigned)app->icon_size, 0, CopyFromParent,
                              InputOutput, CopyFromParent,
                              CWBackPixmap | CWEventMask, &attr);
    slot->gc = XCreateGC(app->dpy, slot->win, 0, NULL);
    slot->w = slot->h = app->icon_size;
    slot->active = 1;
    slot->bg_dirty = 1;

    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize | PBaseSize;
    hints.min_width = hints.min_height = 16;
    hints.base_width = hints.base_height = app->icon_size;
    XSetWMNormalHints(app->dpy, slot->win, &hints);
    XSetClassHint(app->dpy, slot->win, &klass);
    XChangeProperty(app->dpy, slot->win, app->xembed_info, app->xembed_info, 32,
                    PropModeReplace, (unsigned char *)embed, 2);
    XChangeProperty(app->dpy, slot->win, app->net_wm_pid, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&pid, 1);
    {
        unsigned long self = (unsigned long)slot->win;

        XChangeProperty(app->dpy, slot->win, app->kde_tray_for, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *)&self, 1);
    }
    set_tip(app, slot, app->tips[metric][0] ? app->tips[metric] : "traystats");
    dock_request(app, slot);
}

static void slot_destroy(App *app, int metric)
{
    Slot *slot = &app->slots[metric];

    if (!slot->active)
        return;
    XFreeGC(app->dpy, slot->gc);
    XDestroyWindow(app->dpy, slot->win);
    if (slot->img)
        XDestroyImage(slot->img);
    free(slot->cv.px);
    free(slot->bg);
    memset(slot, 0, sizeof(*slot));
}

static int slot_fetch_bg(App *app, Slot *slot)
{
    XImage *img;
    int x, y;

    x_error_seen = 0;
    XClearWindow(app->dpy, slot->win); /* paints the tray's own background */
    XSync(app->dpy, False);
    img = XGetImage(app->dpy, slot->win, 0, 0, (unsigned)slot->w,
                    (unsigned)slot->h, AllPlanes, ZPixmap);
    XSync(app->dpy, False);
    if (!img || x_error_seen) {
        if (img)
            XDestroyImage(img);
        return -1;
    }
    for (y = 0; y < slot->h; y++)
        for (x = 0; x < slot->w; x++)
            unpack_rgb(&app->fmt, XGetPixel(img, x, y),
                       slot->bg + ((size_t)y * (size_t)slot->w + (size_t)x) * 3);
    XDestroyImage(img);
    return 0;
}

static void slot_render(App *app, int metric)
{
    Slot *slot = &app->slots[metric];
    double now = now_sec();

    if (!slot->active || !slot->docked || !slot->mapped || slot->w < 4 ||
        slot->h < 4)
        return;
    if (!slot->img || slot->img->width != slot->w ||
        slot->img->height != slot->h) {
        if (slot->img)
            XDestroyImage(slot->img);
        free(slot->bg);
        slot->img = image_create(app, slot->w, slot->h);
        slot->bg = calloc((size_t)slot->w * (size_t)slot->h, 3);
        slot->bg_dirty = 1;
        if (!slot->img || !slot->bg || canvas_resize(&slot->cv, slot->w, slot->h)) {
            if (slot->img)
                XDestroyImage(slot->img);
            slot->img = NULL;
            return;
        }
    }
    if ((slot->bg_dirty && now - slot->bg_time > BG_MIN_GAP_SEC) ||
        now - slot->bg_time > (slot->bg_ok ? BG_REFRESH_SEC : 1.0)) {
        slot->bg_ok = slot_fetch_bg(app, slot) == 0;
        slot->bg_dirty = 0;
        slot->bg_time = now;
        if (!slot->bg_ok) /* neutral stand-in until the real one is readable */
            memset(slot->bg, 0x30, (size_t)slot->w * (size_t)slot->h * 3);
    }
    render_metric(&slot->cv, metric, &app->anim, &app->stats);
    canvas_to_image(app, &slot->cv, slot->bg, slot->img);
    XPutImage(app->dpy, slot->win, slot->gc, slot->img, 0, 0, 0, 0,
              (unsigned)slot->w, (unsigned)slot->h);
}

/* ---------------------------------------------------------------------- */
/*  Configuration                                                         */
/* ---------------------------------------------------------------------- */

static int config_path(char *buf, size_t size, int make_dir)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char dir[PATH_MAX];

    if (xdg && *xdg)
        snprintf(dir, sizeof(dir), "%s", xdg);
    else if (home && *home)
        snprintf(dir, sizeof(dir), "%s/.config", home);
    else
        return -1;
    if (make_dir)
        mkdir(dir, 0755);
    return snprintf(buf, size, "%s/traystats.conf", dir) >= (int)size ? -1 : 0;
}

static void config_load(App *app)
{
    static const char *const keys[METRIC_COUNT] = {"cpu", "ram", "io"};
    char path[PATH_MAX], line[128];
    FILE *file;
    int any = 0, i;

    for (i = 0; i < METRIC_COUNT; i++)
        app->enabled[i] = 1;
    if (config_path(path, sizeof(path), 0) != 0 || !(file = fopen(path, "r")))
        return;
    while (fgets(line, sizeof(line), file)) {
        for (i = 0; i < METRIC_COUNT; i++) {
            size_t n = strlen(keys[i]);

            if (strncmp(line, keys[i], n) == 0 && line[n] == '=')
                app->enabled[i] = atoi(line + n + 1) != 0;
        }
    }
    fclose(file);
    for (i = 0; i < METRIC_COUNT; i++)
        any |= app->enabled[i];
    if (!any) /* never start with nothing to click on */
        for (i = 0; i < METRIC_COUNT; i++)
            app->enabled[i] = 1;
}

static void config_save(const App *app)
{
    char path[PATH_MAX];
    FILE *file;

    if (config_path(path, sizeof(path), 1) != 0 || !(file = fopen(path, "w")))
        return;
    fprintf(file, "cpu=%d\nram=%d\nio=%d\n", app->enabled[METRIC_CPU],
            app->enabled[METRIC_RAM], app->enabled[METRIC_IO]);
    fclose(file);
}

/* Create the enabled slots in order so the tray lists them consistently. */
static void slots_apply(App *app, int first)
{
    int i;

    for (i = first; i < METRIC_COUNT; i++)
        slot_destroy(app, i);
    for (i = first; i < METRIC_COUNT; i++) {
        if (!app->enabled[i])
            continue;
        slot_create(app, i);
        XSync(app->dpy, False);
    }
}

static int enabled_count(const App *app)
{
    int i, n = 0;

    for (i = 0; i < METRIC_COUNT; i++)
        n += app->enabled[i] ? 1 : 0;
    return n;
}

static void toggle_metric(App *app, int metric)
{
    if (app->enabled[metric]) {
        if (enabled_count(app) <= 1)
            return; /* keep one icon alive so the menu stays reachable */
        app->enabled[metric] = 0;
        slot_destroy(app, metric);
    } else {
        app->enabled[metric] = 1;
        slots_apply(app, metric); /* rebuild later slots to preserve order */
    }
    config_save(app);
}

/* ---------------------------------------------------------------------- */
/*  Tooltips                                                              */
/* ---------------------------------------------------------------------- */

static void build_tips(const Stats *st, char tips[METRIC_COUNT][160])
{
    if (st->has_rpm && st->has_temp)
        snprintf(tips[METRIC_CPU], 160, "CPU fan: %.0f RPM, %.1f C, load %.0f%%",
                 st->rpm, st->temp, st->cpu_load * 100.0);
    else if (st->has_rpm)
        snprintf(tips[METRIC_CPU], 160, "CPU fan: %.0f RPM, load %.0f%%",
                 st->rpm, st->cpu_load * 100.0);
    else if (st->has_temp)
        snprintf(tips[METRIC_CPU], 160,
                 "CPU: %.1f C, load %.0f%% (no fan sensor)", st->temp,
                 st->cpu_load * 100.0);
    else
        snprintf(tips[METRIC_CPU], 160, "CPU fan: sensor unavailable, load %.0f%%",
                 st->cpu_load * 100.0);
    snprintf(tips[METRIC_RAM], 160, "Memory: %.1f / %.1f GiB (%.0f%%)",
             st->ram_used_gib, st->ram_total_gib, st->ram_used * 100.0);
    snprintf(tips[METRIC_IO], 160, "Disk I/O: read %.1f MB/s, write %.1f MB/s",
             st->read_mbs, st->write_mbs);
}

/* ---------------------------------------------------------------------- */
/*  Pop-up menu                                                           */
/* ---------------------------------------------------------------------- */

static int menu_row_y(int row)
{
    return row < METRIC_COUNT ? MENU_PAD + row * MENU_ROW
                              : MENU_PAD + METRIC_COUNT * MENU_ROW + MENU_SEP;
}

static int menu_row_at(int y)
{
    int row;

    for (row = 0; row <= ROW_QUIT; row++)
        if (y >= menu_row_y(row) && y < menu_row_y(row) + MENU_ROW)
            return row;
    return -1;
}

static void menu_render(App *app)
{
    Menu *menu = &app->menu;
    Canvas *cv = &menu->cv;
    Anim demo = app->anim;
    int row, x, y, last_one = enabled_count(app) <= 1;

    canvas_resize(cv, MENU_W, MENU_H);
    demo.has_temp = 1;
    demo.temp = 40.0;
    demo.ram = 0.55;
    demo.load = 0.45;
    demo.act = 0.5;
    demo.arm = 0.45;
    demo.fan_rps = 0.4;

    for (y = 0; y < MENU_H; y++) {
        for (x = 0; x < MENU_W; x++) {
            Acc A = {0, 0, 0, 0};
            int edge = x == 0 || y == 0 || x == MENU_W - 1 || y == MENU_H - 1;

            over(&A, mixc(hex(0x232831), hex(0x161a21), (double)y / MENU_H), 1.0);
            if (edge)
                over(&A, hex(0x48505e), 1.0);
            else if (y == MENU_PAD + METRIC_COUNT * MENU_ROW + MENU_SEP / 2 &&
                     x > MENU_PAD && x < MENU_W - MENU_PAD)
                over(&A, WHITE, 0.10);
            store(cv, x, y, &A);
        }
    }
    for (row = 0; row <= ROW_QUIT; row++) {
        int ry = menu_row_y(row);
        int on = row < METRIC_COUNT ? app->enabled[row] : 1;
        double fy = ry + MENU_ROW / 2.0;

        if (menu->hover == row)
            for (y = ry; y < ry + MENU_ROW; y++)
                for (x = 4; x < MENU_W - 4; x++) {
                    double d = sd_rrect(x + 0.5 - MENU_W / 2.0, y + 0.5 - fy,
                                        MENU_W / 2.0 - 4.0, MENU_ROW / 2.0 - 1.0,
                                        6.0);
                    float *p = cv->px + ((size_t)y * MENU_W + (size_t)x) * 4;
                    double a = 0.09 * cov(d);

                    p[0] = (float)(p[0] * (1 - a) + a);
                    p[1] = (float)(p[1] * (1 - a) + a);
                    p[2] = (float)(p[2] * (1 - a) + a);
                }
        if (row < METRIC_COUNT) {
            Canvas icon = {0, 0, NULL};
            double sw_x = MENU_W - 14.0 - 17.0;

            canvas_resize(&icon, 24, 24);
            render_metric(&icon, row, &demo, &app->stats);
            blit(cv, &icon, 12, ry + 4, on ? 1.0 : 0.35);
            free(icon.px);
            /* toggle switch */
            for (y = ry; y < ry + MENU_ROW; y++)
                for (x = (int)sw_x - 20; x < (int)sw_x + 20; x++) {
                    double d = sd_rrect(x + 0.5 - sw_x, y + 0.5 - fy, 17.0, 9.0,
                                        9.0);
                    double kd = hypot(x + 0.5 - (sw_x + (on ? 8.0 : -8.0)),
                                      y + 0.5 - fy) - 6.5;
                    Rgb track = on ? hex(last_one ? 0x2f8f60 : 0x37c97c)
                                   : hex(0x3a414c);
                    Rgb knob = on ? hex(0xf4fff8) : hex(0x9aa3b0);
                    float *p = cv->px + ((size_t)y * MENU_W + (size_t)x) * 4;
                    Acc A = {p[0], p[1], p[2], p[3]};

                    over(&A, track, cov(d));
                    over(&A, BLACK, 0.30 * cov(kd - 0.8) * cov(d));
                    over(&A, knob, cov(kd));
                    store(cv, x, y, &A);
                }
        } else {
            /* power glyph */
            for (y = ry; y < ry + MENU_ROW; y++)
                for (x = 10; x < 38; x++) {
                    double gx = x + 0.5 - 24.0, gy = y + 0.5 - fy - 0.5;
                    double r = hypot(gx, gy);
                    double ang = atan2(gx, -gy);
                    double ring = fabs(r - 6.2) - 1.0;
                    double bar = sd_rrect(gx, gy + 5.0, 1.0, 5.0, 1.0);
                    double d = fabs(ang) < 0.55 ? 99.0 : ring;
                    float *p = cv->px + ((size_t)y * MENU_W + (size_t)x) * 4;
                    Acc A = {p[0], p[1], p[2], p[3]};

                    over(&A, hex(0xff7a6b), fmax(cov(d), cov(bar)));
                    store(cv, x, y, &A);
                }
        }
    }
    canvas_to_image(app, cv, NULL, menu->img);
}

static void menu_paint(App *app)
{
    Menu *menu = &app->menu;
    int row, ascent = menu->font ? menu->font->ascent : 10,
        descent = menu->font ? menu->font->descent : 3;

    XPutImage(app->dpy, menu->win, menu->gc, menu->img, 0, 0, 0, 0, MENU_W,
              MENU_H);
    for (row = 0; row <= ROW_QUIT; row++) {
        const char *label = row < METRIC_COUNT ? metric_label[row] : "Quit traystats";
        int on = row < METRIC_COUNT ? app->enabled[row] : 1;
        int y = menu_row_y(row) + (MENU_ROW + ascent - descent) / 2;

        XSetForeground(app->dpy, menu->gc,
                       row == ROW_QUIT ? menu->quit : (on ? menu->fg : menu->dim));
        XDrawString(app->dpy, menu->win, menu->gc, 44, y, label,
                    (int)strlen(label));
    }
}

static void menu_close(App *app)
{
    Menu *menu = &app->menu;

    if (!menu->open)
        return;
    XUngrabPointer(app->dpy, CurrentTime);
    XUngrabKeyboard(app->dpy, CurrentTime);
    XUnmapWindow(app->dpy, menu->win);
    XFlush(app->dpy);
    menu->open = 0;
}

static void menu_open(App *app, int metric)
{
    Menu *menu = &app->menu;
    Slot *slot = &app->slots[metric];
    Window child;
    int ix, iy, mx, my;
    int sw = DisplayWidth(app->dpy, app->screen);
    int sh = DisplayHeight(app->dpy, app->screen);

    if (!slot->active)
        return;
    XTranslateCoordinates(app->dpy, slot->win, app->root, 0, 0, &ix, &iy, &child);
    mx = ix + slot->w / 2 - MENU_W / 2;
    my = iy - MENU_H - 4;
    if (my < 0)
        my = iy + slot->h + 4;
    if (my + MENU_H > sh)
        my = sh - MENU_H;
    if (mx + MENU_W > sw)
        mx = sw - MENU_W;
    if (mx < 0)
        mx = 0;
    if (my < 0)
        my = 0;
    menu->hover = -1;
    menu_render(app);
    XMoveWindow(app->dpy, menu->win, mx, my);
    XMapRaised(app->dpy, menu->win);
    if (XGrabPointer(app->dpy, menu->win, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, None,
                     CurrentTime) != GrabSuccess) {
        XUnmapWindow(app->dpy, menu->win);
        return;
    }
    XGrabKeyboard(app->dpy, menu->win, False, GrabModeAsync, GrabModeAsync,
                  CurrentTime);
    menu->open = 1;
    menu_paint(app);
    XFlush(app->dpy);
}

static void menu_button(App *app, int x, int y)
{
    int row;

    if (x < 0 || y < 0 || x >= MENU_W || y >= MENU_H) {
        menu_close(app);
        return;
    }
    row = menu_row_at(y);
    if (row == ROW_QUIT) {
        running = 0;
    } else if (row >= 0) {
        toggle_metric(app, row);
        menu_render(app);
        menu_paint(app);
        XFlush(app->dpy);
    }
}

static void menu_create(App *app)
{
    Menu *menu = &app->menu;
    XSetWindowAttributes attr;
    XColor color;
    static const char *const fonts[] = {
        "-*-helvetica-medium-r-normal--12-*-*-*-p-*-iso8859-1",
        "-*-*-medium-r-normal-sans-12-*-*-*-*-*-iso8859-1",
        "-*-*-medium-r-*-sans-13-*-*-*-*-*-iso8859-1", "fixed"};
    size_t i;

    attr.override_redirect = True;
    attr.background_pixmap = None;
    attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | KeyPressMask;
    menu->win = XCreateWindow(app->dpy, app->root, 0, 0, MENU_W, MENU_H, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWOverrideRedirect | CWBackPixmap | CWEventMask,
                              &attr);
    menu->gc = XCreateGC(app->dpy, menu->win, 0, NULL);
    for (i = 0; i < sizeof(fonts) / sizeof(fonts[0]) && !menu->font; i++)
        menu->font = XLoadQueryFont(app->dpy, fonts[i]);
    if (menu->font)
        XSetFont(app->dpy, menu->gc, menu->font->fid);
    menu->img = image_create(app, MENU_W, MENU_H);
#define ALLOC_COLOR(slot, text)                                               \
    do {                                                                      \
        XParseColor(app->dpy, DefaultColormap(app->dpy, app->screen), text,   \
                    &color);                                                  \
        XAllocColor(app->dpy, DefaultColormap(app->dpy, app->screen), &color); \
        (slot) = color.pixel;                                                 \
    } while (0)
    ALLOC_COLOR(menu->fg, "#e8edf4");
    ALLOC_COLOR(menu->dim, "#7e8794");
    ALLOC_COLOR(menu->quit, "#ff9d90");
#undef ALLOC_COLOR
    menu->hover = -1;
}

/* ---------------------------------------------------------------------- */
/*  Events                                                                */
/* ---------------------------------------------------------------------- */

static int slot_of(App *app, Window win)
{
    int i;

    for (i = 0; i < METRIC_COUNT; i++)
        if (app->slots[i].active && app->slots[i].win == win)
            return i;
    return -1;
}

static void handle_event(App *app, XEvent *ev)
{
    Menu *menu = &app->menu;
    int m;

    if (ev->xany.window == menu->win) {
        if (ev->type == Expose && ev->xexpose.count == 0 && menu->open) {
            menu_paint(app);
        } else if (ev->type == MotionNotify && menu->open) {
            int row = menu_row_at(ev->xmotion.y);

            if (ev->xmotion.x < 0 || ev->xmotion.x >= MENU_W)
                row = -1;
            if (row != menu->hover) {
                menu->hover = row;
                menu_render(app);
                menu_paint(app);
            }
        } else if (ev->type == ButtonPress && menu->open) {
            menu_button(app, ev->xbutton.x, ev->xbutton.y);
        } else if (ev->type == KeyPress && menu->open) {
            if (XLookupKeysym(&ev->xkey, 0) == XK_Escape)
                menu_close(app);
        }
        return;
    }

    if (ev->type == ClientMessage && ev->xclient.message_type == app->manager &&
        (Atom)ev->xclient.data.l[1] == app->tray_atom) {
        for (m = 0; m < METRIC_COUNT; m++)
            if (app->slots[m].active && !app->slots[m].docked)
                app->slots[m].dock_time = 0.0;
        app->last_tray_poll = 0.0;
        return;
    }

    m = slot_of(app, ev->xany.window);
    if (m < 0)
        return;
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count == 0)
            app->slots[m].bg_dirty = 1;
        break;
    case ConfigureNotify:
        if (ev->xconfigure.width > 0 && ev->xconfigure.height > 0) {
            app->slots[m].w = ev->xconfigure.width;
            app->slots[m].h = ev->xconfigure.height;
            app->slots[m].bg_dirty = 1;
        }
        break;
    case MapNotify:
        app->slots[m].mapped = 1;
        app->slots[m].bg_dirty = 1;
        break;
    case UnmapNotify:
        app->slots[m].mapped = 0;
        break;
    case ReparentNotify:
        if (ev->xreparent.parent == app->root) {
            /* The tray went away: X maps saved-set windows on the root. */
            app->slots[m].docked = 0;
            app->slots[m].mapped = 0;
            app->slots[m].dock_time = now_sec();
            XUnmapWindow(app->dpy, app->slots[m].win);
        } else {
            app->slots[m].docked = 1;
            app->slots[m].bg_dirty = 1;
        }
        break;
    case DestroyNotify: /* the tray destroyed our icon: make a fresh one */
        slot_destroy(app, m);
        if (app->enabled[m]) {
            slot_create(app, m);
            XSync(app->dpy, False);
        }
        break;
    case ButtonPress:
        if (menu->open)
            menu_close(app);
        else
            menu_open(app, m);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------- */
/*  Main loop                                                             */
/* ---------------------------------------------------------------------- */

static void frame(App *app, double dt, int sample_due)
{
    int m;

    if (sample_due) {
        sample_stats(&app->sampler, &app->stats);
        build_tips(&app->stats, app->tips);
        for (m = 0; m < METRIC_COUNT; m++)
            set_tip(app, &app->slots[m], app->tips[m]);
    }
    anim_step(&app->anim, &app->stats, dt);
    for (m = 0; m < METRIC_COUNT; m++)
        slot_render(app, m);
    XFlush(app->dpy);
}

static void dock_retry(App *app)
{
    double now = now_sec();
    int m;

    if (now - app->last_tray_poll < 1.0)
        return;
    app->last_tray_poll = now;
    for (m = 0; m < METRIC_COUNT; m++) {
        Slot *slot = &app->slots[m];

        if (slot->active && !slot->docked &&
            now - slot->dock_time > DOCK_RETRY_SEC)
            dock_request(app, slot);
    }
}

static void usage(const char *name)
{
    printf("usage: %s [-s SIZE] [-d] [-h]\n"
           "  -s SIZE  initial icon size in pixels (default %d; the tray may "
           "resize it)\n"
           "  -d       print X errors\n"
           "  -h       show this help\n",
           name, DEFAULT_ICON_SIZE);
}

int main(int argc, char **argv)
{
    App app;
    struct sigaction action;
    char tray_name[32];
    double next_frame, last_frame, next_sample = 0.0;
    int opt, i;

    memset(&app, 0, sizeof(app));
    app.icon_size = DEFAULT_ICON_SIZE;
    for (opt = 1; opt < argc; opt++) {
        if (strcmp(argv[opt], "-h") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[opt], "-d") == 0) {
            debug_enabled = 1;
        } else if (strcmp(argv[opt], "-s") == 0 && opt + 1 < argc) {
            app.icon_size = (int)clampd(atof(argv[++opt]), 16, 256);
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);

    app.dpy = XOpenDisplay(NULL);
    if (!app.dpy) {
        fprintf(stderr, "traystats: cannot open DISPLAY\n");
        return EXIT_FAILURE;
    }
    XSetErrorHandler(on_x_error);
    app.screen = DefaultScreen(app.dpy);
    app.root = RootWindow(app.dpy, app.screen);
    app.visual = DefaultVisual(app.dpy, app.screen);
    app.depth = DefaultDepth(app.dpy, app.screen);
    if (app.visual->class != TrueColor) {
        fprintf(stderr, "traystats: a TrueColor visual is required\n");
        XCloseDisplay(app.dpy);
        return EXIT_FAILURE;
    }
    pixfmt_init(&app.fmt, app.visual);

    snprintf(tray_name, sizeof(tray_name), "_NET_SYSTEM_TRAY_S%d", app.screen);
    app.tray_atom = XInternAtom(app.dpy, tray_name, False);
    app.manager = XInternAtom(app.dpy, "MANAGER", False);
    app.opcode = XInternAtom(app.dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    app.xembed_info = XInternAtom(app.dpy, "_XEMBED_INFO", False);
    app.net_wm_name = XInternAtom(app.dpy, "_NET_WM_NAME", False);
    app.utf8 = XInternAtom(app.dpy, "UTF8_STRING", False);
    app.net_wm_pid = XInternAtom(app.dpy, "_NET_WM_PID", False);
    app.kde_tray_for =
        XInternAtom(app.dpy, "_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR", False);
    XSelectInput(app.dpy, app.root, StructureNotifyMask);

    anim_init(&app.anim);
    sensors_scan(&app.sampler.sensors);
    sample_stats(&app.sampler, &app.stats);
    build_tips(&app.stats, app.tips);
    app.anim.ram = app.stats.ram_used;
    app.anim.temp = app.stats.temp;
    config_load(&app);
    menu_create(&app);
    slots_apply(&app, 0);

    last_frame = now_sec();
    next_frame = last_frame;
    next_sample = last_frame + SAMPLE_SEC;
    while (running) {
        double now = now_sec();
        int active = 0;

        while (XPending(app.dpy)) {
            XEvent ev;

            XNextEvent(app.dpy, &ev);
            handle_event(&app, &ev);
        }
        if (!running)
            break;
        now = now_sec();
        if (now >= next_frame) {
            double dt = clampd(now - last_frame, 0.001, 0.25);
            int due = now >= next_sample;

            last_frame = now;
            dock_retry(&app);
            frame(&app, dt, due);
            if (due)
                next_sample = now + SAMPLE_SEC;
            for (i = 0; i < METRIC_COUNT; i++)
                active |= app.slots[i].active && app.slots[i].docked &&
                          app.slots[i].mapped;
            next_frame = now + (active ? FRAME_SEC : IDLE_FRAME_SEC);
        }
        if (!XPending(app.dpy)) {
            double wait = next_frame - now_sec();
            struct timeval tv;
            fd_set fds;
            int fd = ConnectionNumber(app.dpy);

            if (wait < 0.0)
                wait = 0.0;
            tv.tv_sec = (time_t)wait;
            tv.tv_usec = (suseconds_t)((wait - floor(wait)) * 1e6);
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            if (select(fd + 1, &fds, NULL, NULL, &tv) < 0 && errno != EINTR)
                break;
        }
    }

    menu_close(&app);
    for (i = 0; i < METRIC_COUNT; i++)
        slot_destroy(&app, i);
    if (app.menu.font)
        XFreeFont(app.dpy, app.menu.font);
    XFreeGC(app.dpy, app.menu.gc);
    XDestroyWindow(app.dpy, app.menu.win);
    if (app.menu.img)
        XDestroyImage(app.menu.img);
    free(app.menu.cv.px);
    XCloseDisplay(app.dpy);
    return EXIT_SUCCESS;
}
