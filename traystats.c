#define _POSIX_C_SOURCE 200809L

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ICON_SIZE 72
#define UPDATE_USEC 100000
#define PI 3.14159265358979323846
#define MENU_WIDTH 150
#define MENU_HEIGHT 92

static volatile sig_atomic_t running = 1;

enum metric {
    METRIC_CPU,
    METRIC_RAM,
    METRIC_IO,
    METRIC_COUNT
};

static void stop_running(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int read_number(const char *path, double *value)
{
    FILE *file = fopen(path, "r");
    char line[128];

    if (!file)
        return -1;
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    *value = strtod(line, NULL);
    return 0;
}

static int read_sensor(const char *prefix, const char *suffix, double *result)
{
    DIR *hwmon = opendir("/sys/class/hwmon");
    struct dirent *entry;
    double value;
    int found = 0;

    if (!hwmon)
        return -1;
    while ((entry = readdir(hwmon)) != NULL) {
        char directory[PATH_MAX];
        char path[PATH_MAX];
        DIR *sensor_dir;
        struct dirent *sensor;

        if (strncmp(entry->d_name, "hwmon", 5) != 0)
            continue;
        if (snprintf(directory, sizeof(directory), "/sys/class/hwmon/%s",
                     entry->d_name) >= (int)sizeof(directory))
            continue;
        sensor_dir = opendir(directory);
        if (!sensor_dir)
            continue;
        while ((sensor = readdir(sensor_dir)) != NULL) {
            if (strncmp(sensor->d_name, prefix, strlen(prefix)) != 0)
                continue;
            if (!strstr(sensor->d_name, suffix))
                continue;
            if (snprintf(path, sizeof(path), "%s/%s", directory,
                         sensor->d_name) >= (int)sizeof(path))
                continue;
            if (read_number(path, &value) == 0 && value >= 0) {
                if (!found || value > *result)
                    *result = value;
                found = 1;
            }
        }
        closedir(sensor_dir);
    }
    closedir(hwmon);
    return found ? 0 : -1;
}

static int read_fan_rpm(double *rpm)
{
    return read_sensor("fan", "_input", rpm);
}

static int read_temperature(double *temperature)
{
    DIR *hwmon = opendir("/sys/class/hwmon");
    struct dirent *entry;
    double best = -1.0;

    if (!hwmon)
        return -1;
    while ((entry = readdir(hwmon)) != NULL) {
        char directory[PATH_MAX];
        DIR *sensor_dir;
        struct dirent *sensor;

        if (strncmp(entry->d_name, "hwmon", 5) != 0)
            continue;
        if (snprintf(directory, sizeof(directory), "/sys/class/hwmon/%s",
                     entry->d_name) >= (int)sizeof(directory))
            continue;
        sensor_dir = opendir(directory);
        if (!sensor_dir)
            continue;
        while ((sensor = readdir(sensor_dir)) != NULL) {
            char path[PATH_MAX];
            double value;

            if (strncmp(sensor->d_name, "temp", 4) != 0 ||
                !strstr(sensor->d_name, "_input"))
                continue;
            if (snprintf(path, sizeof(path), "%s/%s", directory,
                         sensor->d_name) >= (int)sizeof(path))
                continue;
            if (read_number(path, &value) == 0 && value >= 0.0 &&
                value <= 110000.0 && value / 1000.0 > best)
                best = value / 1000.0;
        }
        closedir(sensor_dir);
    }
    closedir(hwmon);
    if (best >= 0.0) {
        *temperature = best;
        return 0;
    }
    return -1;
}

static double read_cpu_load(void)
{
    static unsigned long long previous_total;
    static unsigned long long previous_idle;
    FILE *file = fopen("/proc/stat", "r");
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    unsigned long long total;
    double load = 0.0;

    if (!file)
        return 0.0;
    if (fscanf(file, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user,
               &nice, &system, &idle, &iowait, &irq, &softirq, &steal) == 8) {
        total = user + nice + system + idle + iowait + irq + softirq + steal;
        if (previous_total && total > previous_total &&
            idle + iowait >= previous_idle) {
            load = 1.0 - (double)(idle + iowait - previous_idle) /
                            (double)(total - previous_total);
        }

        previous_total = total;
        previous_idle = idle + iowait;
    }
    fclose(file);
    return load < 0.0 ? 0.0 : (load > 1.0 ? 1.0 : load);
}

static double read_ram_used(void)
{
    FILE *file = fopen("/proc/meminfo", "r");
    char name[32];
    unsigned long long value;
    unsigned long long total = 0;
    unsigned long long available = 0;

    if (!file)
        return 0.0;
    while (fscanf(file, "%31s %llu kB", name, &value) == 2) {
        if (strcmp(name, "MemTotal:") == 0)
            total = value;
        else if (strcmp(name, "MemAvailable:") == 0)
            available = value;
    }
    fclose(file);
    return total ? 1.0 - (double)available / (double)total : 0.0;
}

static double read_io_rate(void)
{
    static unsigned long long previous_sectors;
    static struct timespec previous_time;
    FILE *file = fopen("/proc/diskstats", "r");
    char device[32];
    unsigned int major, minor;
    unsigned long long reads, merged_reads, sectors_read, read_ticks;
    unsigned long long writes, merged_writes, sectors_written, write_ticks;
    unsigned long long in_flight, io_ticks, weighted_ticks;
    unsigned long long sectors = 0;
    struct timespec now;
    double seconds;
    double rate;

    if (!file)
        return 0.0;
    while (fscanf(file, "%u %u %31s %llu %llu %llu %llu %llu %llu %llu %llu "
                       "%llu %llu %llu",
                  &major, &minor, device, &reads, &merged_reads,
                  &sectors_read, &read_ticks, &writes, &merged_writes,
                  &sectors_written, &write_ticks, &in_flight, &io_ticks,
                  &weighted_ticks) == 14) {
        if (strstr(device, "loop") || strstr(device, "ram") ||
            strstr(device, "zram"))
            continue;
        sectors += sectors_read + sectors_written;
    }
    fclose(file);
    clock_gettime(CLOCK_MONOTONIC, &now);
    seconds = previous_time.tv_sec
                  ? (double)(now.tv_sec - previous_time.tv_sec) +
                        (double)(now.tv_nsec - previous_time.tv_nsec) /
                            1000000000.0
                  : 0.0;
    rate = seconds > 0.0 && sectors >= previous_sectors
               ? (double)(sectors - previous_sectors) / seconds / 2048.0
               : 0.0;
    previous_sectors = sectors;
    previous_time = now;
    return rate > 1.0 ? 1.0 : rate;
}

static unsigned long color_for_temperature(Display *display, Colormap colormap,
                                           double temperature)
{
    XColor color;
    const char *name;

    if (temperature < 50.0)
        name = "#4caf50";
    else if (temperature < 70.0)
        name = "#f0b429";
    else
        name = "#e05252";
    if (!XParseColor(display, colormap, name, &color) ||
        !XAllocColor(display, colormap, &color))
        return BlackPixel(display, DefaultScreen(display));
    return color.pixel;
}

static void draw_thermostat(Display *display, Window window, GC gc,
                            double temperature, int compact)
{
    struct timespec now;
    double blink_period;
    double elapsed;
    XWindowAttributes attributes;
    int x;
    int y;
    unsigned long color;

    if (temperature < 70.0 || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return;
    blink_period = temperature > 90.0 ? 0.18 : 0.75;
    elapsed = (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
    if (fmod(elapsed, blink_period * 2.0) >= blink_period)
        return;

    XGetWindowAttributes(display, window, &attributes);
    x = compact ? attributes.width / 2 - 9 : attributes.width - 9;
    y = 4;
    color = temperature > 90.0
                ? color_for_temperature(display,
                                        DefaultColormap(display, 0), 100.0)
                : color_for_temperature(display,
                                        DefaultColormap(display, 0), 70.0);
    XSetForeground(display, gc, color);
    XSetLineAttributes(display, gc, 2, LineSolid, CapRound, JoinRound);
    XDrawLine(display, window, gc, x, y, x, y + 9);
    XDrawArc(display, window, gc, x - 3, y + 7, 6, 6, 0, 360 * 64);
    XFillArc(display, window, gc, x - 2, y + 8, 4, 4, 0, 360 * 64);
    XSetLineAttributes(display, gc, 1, LineSolid, CapButt, JoinMiter);
}

static void draw_memory_graph(Display *display, Window window, GC gc,
                              double used, int compact)
{
    XWindowAttributes attributes;
    static double history[16];
    XPoint points[16];
    unsigned long color;
    int i;
    int x;
    int y;
    int marker_x;

    XGetWindowAttributes(display, window, &attributes);
    for (i = 0; i < 15; i++)
        history[i] = history[i + 1];
    history[15] = used;
    color = color_for_temperature(display, DefaultColormap(display, 0), 50.0);
    x = compact ? (attributes.width * 3) / 4 - 15
                : (attributes.width - 18) / 2;
    y = compact ? 4 : (attributes.height - 14) / 2;
    XSetForeground(display, gc, color);
    XDrawRectangle(display, window, gc, x, y, compact ? 31 : 17,
                   compact ? 24 : 13);
    for (i = 0; i < 16; i++) {
        points[i].x = (short)(x + 1 + i * (compact ? 2 : 1));
        points[i].y = (short)(y + (compact ? 20 : 11) -
                              history[i] * (compact ? 17 : 9));
    }
    XDrawLines(display, window, gc, points, 16, CoordModeOrigin);
    marker_x = x + 1 + ((int)(used * (compact ? 15 : 10)) %
                              (compact ? 16 : 11));
    XDrawLine(display, window, gc, marker_x, y + 2,
              marker_x, y + (compact ? 12 : 10));
}

static void draw_disk_icon(Display *display, Window window, GC gc,
                           double activity, double phase, int compact)
{
    XWindowAttributes attributes;
    unsigned long color;
    int center_x;
    int center_y;
    int orbit_radius;
    int orbit_width;
    int satellite_x;
    int satellite_y;
    double satellite_angle;

    XGetWindowAttributes(display, window, &attributes);
    color = activity > 0.02
                ? color_for_temperature(display,
                                        DefaultColormap(display, 0), 70.0)
                : 0x8a929b;
    center_x = compact ? (attributes.width * 3) / 4
                       : attributes.width / 2;
    center_y = compact ? (attributes.height * 3) / 4
                       : attributes.height / 2;

    /*
     * The central disk stays fixed; only the separate planetary orbit
     * breathes with I/O activity and rotates slowly around it.
     */
    orbit_radius = (attributes.width < 30 || attributes.height < 30)
                       ? 5 + (int)(activity * 3.0)
                       : 8 + (int)(activity * 5.0);
    orbit_width = orbit_radius * 2;
    XSetForeground(display, gc, color);
    XSetLineAttributes(display, gc, 3, LineSolid, CapRound, JoinRound);
    XDrawArc(display, window, gc, center_x - orbit_radius,
             center_y - orbit_radius, orbit_width, orbit_width, 0, 360 * 64);
    XSetLineAttributes(display, gc, 1, LineSolid, CapButt, JoinMiter);
    XFillArc(display, window, gc, center_x - 5, center_y - 5, 10, 10, 0,
             360 * 64);
    satellite_angle = phase * (0.5 + activity * 10.0);
    satellite_x = center_x + (int)(cos(satellite_angle) * orbit_radius);
    satellite_y = center_y + (int)(sin(satellite_angle) * orbit_radius);
    XSetForeground(display, gc, color);
    XFillArc(display, window, gc, satellite_x - 3, satellite_y - 3, 6, 6, 0,
             360 * 64);
    XSetForeground(display, gc, BlackPixel(display, 0));
    XFillArc(display, window, gc, center_x - 2, center_y - 2, 4, 4, 0,
             360 * 64);
    XSetLineAttributes(display, gc, 1, LineSolid, CapButt, JoinMiter);
}

static void draw_fan(Display *display, Window window, GC gc, double phase,
                     double temperature, int cpu_enabled, int ram_enabled,
                     int io_enabled, double ram_used, double io_rate)
{
    XWindowAttributes attributes;
    XPoint blade[5];
    double angle;
    int center;
    int radius;
    int i;
    int compact;
    unsigned long fan_color;

    XGetWindowAttributes(display, window, &attributes);
    compact = (cpu_enabled && ram_enabled) || (cpu_enabled && io_enabled);
    center = compact ? attributes.width / 4 : attributes.width / 2;
    radius = compact ? attributes.width / 4 - 4
                     : (attributes.width < attributes.height
                            ? attributes.width / 2 - 2
                            : attributes.height / 2 - 2);
    if (radius < 4)
        radius = 4;
    fan_color = color_for_temperature(display, DefaultColormap(display, 0),
                                       temperature);
    XClearWindow(display, window);
    if (!cpu_enabled)
        goto indicators;
    angle = fmod(phase, 2.0 * PI);
    /*
     * Five broad, swept spokes give the icon a car-rim shape. There is no
     * enclosing circle, so the panel remains visible between the blades.
     */
    XSetForeground(display, gc, fan_color);
    for (i = 0; i < 5; i++) {
        double a = angle + i * (2.0 * PI / 5.0);
        double side = 0.24;
        double end = radius * 0.96;
        double shoulder = radius * 0.66;

        blade[0].x = (short)(center + cos(a - side) * 2.5);
        blade[0].y = (short)(center + sin(a - side) * 2.5);
        blade[1].x = (short)(center + cos(a - side) * shoulder);
        blade[1].y = (short)(center + sin(a - side) * shoulder);
        blade[2].x = (short)(center + cos(a + side * 0.25) * end);
        blade[2].y = (short)(center + sin(a + side * 0.25) * end);
        blade[3].x = (short)(center + cos(a + side) * end);
        blade[3].y = (short)(center + sin(a + side) * end);
        blade[4].x = (short)(center + cos(a + side * 0.15) * 4.0);
        blade[4].y = (short)(center + sin(a + side * 0.15) * 4.0);
        XFillPolygon(display, window, gc, blade, 5, Convex, CoordModeOrigin);
    }

    XSetForeground(display, gc, BlackPixel(display, 0));
    XFillArc(display, window, gc, center - 3, center - 3, 6, 6, 0, 360 * 64);
    draw_thermostat(display, window, gc, temperature, compact);
indicators:
    if (ram_enabled)
        draw_memory_graph(display, window, gc, ram_used,
                          cpu_enabled || io_enabled);
    if (io_enabled)
        draw_disk_icon(display, window, gc, io_rate, phase,
                       cpu_enabled || ram_enabled);
}

static void draw_slot(Display *display, Window window, GC gc, int metric,
                      double phase, double temperature, double ram_used,
                      double io_rate)
{
    XClearWindow(display, window);
    if (metric == METRIC_CPU)
        draw_fan(display, window, gc, phase, temperature, 1, 0, 0, 0.0, 0.0);
    else if (metric == METRIC_RAM)
        draw_memory_graph(display, window, gc, ram_used, 0);
    else
        draw_disk_icon(display, window, gc, io_rate, phase, 0);
}

static Window tray_owner(Display *display, Atom tray_atom)
{
    return XGetSelectionOwner(display, tray_atom);
}

static int dock_in_tray(Display *display, Window icon, Atom tray_atom)
{
    Window owner = tray_owner(display, tray_atom);
    XClientMessageEvent event;

    if (owner == None)
        return -1;
    memset(&event, 0, sizeof(event));
    event.type = ClientMessage;
    event.window = owner;
    event.message_type = XInternAtom(display, "_NET_SYSTEM_TRAY_OPCODE", False);
    event.format = 32;
    event.data.l[0] = CurrentTime;
    event.data.l[1] = 0; /* SYSTEM_TRAY_REQUEST_DOCK */
    event.data.l[2] = icon;
    XSendEvent(display, owner, False, NoEventMask, (XEvent *)&event);
    XSync(display, False);
    return 0;
}

static void draw_menu(Display *display, Window menu, GC gc,
                      const int enabled[METRIC_COUNT])
{
    static const char *labels[METRIC_COUNT] = {"CPU / fan", "Memory", "Disk I/O"};
    int i;

    XSetForeground(display, gc, 0x20252b);
    XFillRectangle(display, menu, gc, 0, 0, MENU_WIDTH, MENU_HEIGHT);
    XSetForeground(display, gc, 0xd7dde5);
    for (i = 0; i < METRIC_COUNT; i++) {
        int y = 8 + i * 26;
        XDrawRectangle(display, menu, gc, 10, y, 12, 12);
        if (enabled[i]) {
            XSetForeground(display, gc, 0x55c878);
            XFillRectangle(display, menu, gc, 13, y + 3, 7, 7);
            XSetForeground(display, gc, 0xd7dde5);
        }
        XDrawString(display, menu, gc, 32, y + 11, labels[i],
                    (int)strlen(labels[i]));
    }
}

static void show_menu(Display *display, Window menu, Window icon)
{
    XWindowAttributes attributes;
    Window root;
    int x;
    int y;

    XGetWindowAttributes(display, icon, &attributes);
    XTranslateCoordinates(display, icon, RootWindow(display, DefaultScreen(display)),
                          0, 0, &x, &y, &root);
    XMoveWindow(display, menu, x - MENU_WIDTH + attributes.width, y - MENU_HEIGHT);
    XMapRaised(display, menu);
    XGrabPointer(display, menu, False, ButtonPressMask, GrabModeAsync,
                 GrabModeAsync, None, None, CurrentTime);
}

static void hide_menu(Display *display, Window menu)
{
    XUngrabPointer(display, CurrentTime);
    XUnmapWindow(display, menu);
    XFlush(display);
}

static void set_tooltip(Display *display, Window window, double rpm,
                         double temperature)
{
    char title[128];
    char *title_list[1] = {title};
    XTextProperty property;

    if (rpm >= 0.0 && temperature >= -100.0)
        snprintf(title, sizeof(title), "CPU fan: %.0f RPM, %.1f C", rpm,
                 temperature);
    else
        snprintf(title, sizeof(title), "CPU fan: sensor unavailable");
    if (XStringListToTextProperty(title_list, 1, &property)) {
        XSetWMName(display, window, &property);
        XFree(property.value);
    }
}

int main(void)
{
    Display *display;
    int screen;
    Window icons[METRIC_COUNT];
    Window menu;
    GC gcs[METRIC_COUNT];
    GC menu_gc;
    Atom tray_atom;
    XSetWindowAttributes attributes;
    XSetWindowAttributes menu_attributes;
    double rpm = 0.0;
    double temperature = 0.0;
    double phase = 0.0;
    double rotation_speed = 0.04;
    double ram_used = 0.0;
    double io_rate = 0.0;
    int enabled[METRIC_COUNT] = {1, 1, 1};
    int menu_open = 0;
    struct timespec previous_time;
    int docked[METRIC_COUNT] = {0, 0, 0};
    struct sigaction signal_action;

    memset(&signal_action, 0, sizeof(signal_action));
    signal_action.sa_handler = stop_running;
    sigemptyset(&signal_action.sa_mask);
    sigaction(SIGINT, &signal_action, NULL);
    sigaction(SIGTERM, &signal_action, NULL);

    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "traystats: cannot open DISPLAY\n");
        return EXIT_FAILURE;
    }
    screen = DefaultScreen(display);
    attributes.background_pixmap = ParentRelative;
    attributes.event_mask = ExposureMask | StructureNotifyMask | ButtonPressMask;
    XClassHint class_hint = {"traystats", "Traystats"};
    for (int metric = 0; metric < METRIC_COUNT; metric++) {
        icons[metric] = XCreateWindow(
            display, RootWindow(display, screen), 0, 0, ICON_SIZE, ICON_SIZE, 0,
            CopyFromParent, InputOutput, CopyFromParent,
            CWBackPixmap | CWEventMask, &attributes);
        XStoreName(display, icons[metric], "traystats");
        XSetClassHint(display, icons[metric], &class_hint);
    }
    for (int metric = 0; metric < METRIC_COUNT; metric++)
        gcs[metric] = XCreateGC(display, icons[metric], 0, NULL);
    menu_attributes.override_redirect = True;
    menu = XCreateWindow(display, RootWindow(display, screen), 0, 0,
                         MENU_WIDTH, MENU_HEIGHT, 1, CopyFromParent,
                         InputOutput, CopyFromParent, CWOverrideRedirect,
                         &menu_attributes);
    XSelectInput(display, menu, ExposureMask | ButtonPressMask);
    menu_gc = XCreateGC(display, menu, 0, NULL);
    tray_atom = XInternAtom(display, "_NET_SYSTEM_TRAY_S0", False);
    XSelectInput(display, RootWindow(display, screen), StructureNotifyMask);
    clock_gettime(CLOCK_MONOTONIC, &previous_time);
    for (int metric = 0; metric < METRIC_COUNT; metric++) {
        docked[metric] = dock_in_tray(display, icons[metric], tray_atom) == 0;
        if (enabled[metric] && docked[metric])
            XMapWindow(display, icons[metric]);
    }

    while (running) {
        struct timeval timeout = {0, UPDATE_USEC};
        fd_set descriptors;
        int connection = ConnectionNumber(display);
        int ready;

        double load = read_cpu_load();
        double target_speed;
        double temperature_factor;
        struct timespec current_time;
        double elapsed;
        int has_rpm = read_fan_rpm(&rpm) == 0;
        ram_used = read_ram_used();
        io_rate = read_io_rate();

        if (!has_rpm)
            rpm = -1.0;
        if (read_temperature(&temperature) != 0)
            temperature = -101.0;
        /*
         * A laptop without fan tachometry still gets a meaningful visual:
         * temperature is the primary speed signal, with load adding response.
         * The quadratic curve makes cool operation quiet and hot operation fast.
         */
        temperature_factor = (temperature - 35.0) / 55.0;
        if (temperature_factor < 0.0)
            temperature_factor = 0.0;
        if (temperature_factor > 1.0)
            temperature_factor = 1.0;
        target_speed = 0.04 + temperature_factor * temperature_factor * 0.75;
        if (!has_rpm)
            target_speed += load * 0.12;
        rotation_speed += (target_speed - rotation_speed) * 0.12;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        elapsed = (double)(current_time.tv_sec - previous_time.tv_sec) +
                  (double)(current_time.tv_nsec - previous_time.tv_nsec) /
                      1000000000.0;
        previous_time = current_time;
        if (elapsed < 0.0 || elapsed > 1.0)
            elapsed = 0.1;
        phase += rotation_speed * elapsed * 10.0;
        for (int metric = 0; metric < METRIC_COUNT; metric++) {
            if (!enabled[metric])
                continue;
            draw_slot(display, icons[metric], gcs[metric], metric, phase,
                      temperature < -100.0 ? 50.0 : temperature, ram_used,
                      io_rate);
        }
        set_tooltip(display, icons[METRIC_CPU], rpm, temperature);
        XFlush(display);
        for (int metric = 0; metric < METRIC_COUNT; metric++) {
            if (!docked[metric])
                docked[metric] =
                    dock_in_tray(display, icons[metric], tray_atom) == 0;
            if (enabled[metric] && docked[metric])
                XMapWindow(display, icons[metric]);
        }

        FD_ZERO(&descriptors);
        FD_SET(connection, &descriptors);
        ready = select(connection + 1, &descriptors, NULL, NULL, &timeout);
        if (ready > 0) {
            while (XPending(display)) {
                XEvent event;
                XNextEvent(display, &event);
                int clicked_metric = -1;
                for (int metric = 0; metric < METRIC_COUNT; metric++)
                    if (event.xany.window == icons[metric])
                        clicked_metric = metric;
                if (event.type == ButtonPress && clicked_metric >= 0) {
                    show_menu(display, menu, icons[clicked_metric]);
                    draw_menu(display, menu, menu_gc, enabled);
                    menu_open = 1;
                } else if (event.type == ButtonPress && menu_open) {
                    XWindowAttributes menu_attributes;
                    Window child;
                    int menu_x;
                    int menu_y;
                    int row;

                    XGetWindowAttributes(display, menu, &menu_attributes);
                    XTranslateCoordinates(display, RootWindow(display, screen),
                                          menu, event.xbutton.x_root,
                                          event.xbutton.y_root, &menu_x,
                                          &menu_y, &child);
                    if (menu_x < 0 || menu_x >= menu_attributes.width ||
                        menu_y < 0 || menu_y >= menu_attributes.height) {
                        hide_menu(display, menu);
                        menu_open = 0;
                        continue;
                    }
                    row = (menu_y - 4) / 26;
                    if (row >= 0 && row < METRIC_COUNT &&
                        menu_y >= 4 + row * 26 &&
                        menu_y < 4 + (row + 1) * 26) {
                        enabled[row] = !enabled[row];
                        XClearWindow(display, menu);
                        draw_menu(display, menu, menu_gc, enabled);
                        for (int metric = 0; metric < METRIC_COUNT; metric++) {
                            if (enabled[metric] && docked[metric])
                                XMapWindow(display, icons[metric]);
                            else
                                XUnmapWindow(display, icons[metric]);
                        }
                        for (int metric = 0; metric < METRIC_COUNT; metric++)
                            if (enabled[metric])
                                draw_slot(display, icons[metric], gcs[metric],
                                          metric, phase,
                                          temperature < -100.0 ? 50.0 : temperature,
                                          ram_used, io_rate);
                        XSync(display, False);
                        XFlush(display);
                    } else {
                        hide_menu(display, menu);
                        menu_open = 0;
                    }
                } else if (event.type == Expose &&
                           event.xany.window == menu) {
                    draw_menu(display, menu, menu_gc, enabled);
                } else if (event.type == Expose) {
                    if (clicked_metric >= 0) {
                        draw_slot(display, event.xany.window,
                                  gcs[clicked_metric], clicked_metric, phase,
                                  temperature < -100.0 ? 50.0 : temperature,
                                  ram_used, io_rate);
                    }
                } else if (event.type == DestroyNotify) {
                    running = 0;
                }
            }
        } else if (ready < 0 && errno != EINTR) {
            running = 0;
        }
    }

    if (menu_open)
        hide_menu(display, menu);
    XFreeGC(display, menu_gc);
    XDestroyWindow(display, menu);
    for (int metric = 0; metric < METRIC_COUNT; metric++)
        XFreeGC(display, gcs[metric]);
    for (int metric = 0; metric < METRIC_COUNT; metric++)
        XDestroyWindow(display, icons[metric]);
    XCloseDisplay(display);
    return EXIT_SUCCESS;
}
