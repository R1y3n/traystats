# traystats

`traystats` is a small C/X11 system-tray monitor for TDE and any other XEmbed
system tray. It gives you up to three independent tray icons, each drawn by a
built-in anti-aliased software renderer, so the artwork stays crisp at any tray
size:

| Slot | What you see |
|------|--------------|
| **CPU / fan** | A glass medallion with a spinning turbine. Rotation speed follows the fan tachometer (RPM); on machines without one it is derived from temperature and CPU load. A ring around the rim shows CPU load. The whole icon is tinted by temperature: green below 50 C, amber 50-70 C, red above 70 C, with smooth blends between. At 70 C a thermometer badge starts blinking, and blinks rapidly above 90 C. |
| **Memory** | A memory chip with gold pins. A liquid fills the window to the current RAM usage, with rippling waves and rising bubbles; it shifts green, amber, red as memory fills. |
| **Disk I/O** | A hard-disk platter with a spinning sheen and a metal actuator arm. Idle, the arm rests parked beside the platter. Under load it seeks across the tracks, and the head and track glow cyan for reads and amber for writes. |

Tooltips show the details: RPM / temperature / load, used and total memory,
and read / write throughput.

Click any icon (either button) to open the menu. It has a switch per slot and
a *Quit* entry. Slots can be turned on and off at any time; a disabled slot is
removed from the tray completely. At least one slot always stays enabled so the
menu remains reachable. `Esc` or a click outside closes the menu. Your choices
are saved in `~/.config/traystats.conf` (or `$XDG_CONFIG_HOME`).

The only dependencies are Xlib and libm. No GTK or Qt.

## Build and run

```sh
make
./traystats &
```

Options: `-s SIZE` sets the initial icon size (the tray may resize it),
`-d` prints X errors for debugging, `-h` shows help. `make install` installs
to `/usr/local/bin` (`PREFIX=` and `DESTDIR=` are honoured).

The process must run inside an X11 session with `DISPLAY` set and a TrueColor
visual. No elevated privileges are needed where the sensor files are readable.
If no hwmon value is available the icon stays visible and the tooltip says so.

## How it behaves

- **Sensors** are discovered once and re-scanned every 30 s. The temperature
  comes from CPU sensors (`coretemp`, `k10temp`, `zenpower`, thermal zones)
  when present, otherwise from the hottest available sensor.
- **Docking** follows the XEmbed tray protocol. If the tray is not running
  yet, or is restarted, `traystats` waits and re-docks automatically.
- **Transparency**: each frame is composited over a snapshot of the tray's own
  background, so icons blend with any panel colour or pseudo-transparent panel.
