# traystats

`traystats` is a small C/X11 system-tray monitor for TDE and other XEmbed
system trays. It scans Linux hwmon devices for the highest available
`fan*_input` value and temperature, then draws a rotating fan icon:

- rotation speed follows the reported fan RPM; on laptops without a fan
  tachometer, CPU load drives a smooth visual fallback;
- green, amber, and red indicate temperatures below 50 C, 50-70 C, and above
  70 C respectively;
- a small blinking thermostat appears at 70 C and blinks rapidly above 90 C;
- the transparent tray background lets the desktop panel show through;
- clicking the tray icon with either mouse button opens CPU/fan, memory, and
  disk-I/O checkboxes; each selected metric gets its own independent tray slot
  and the choices can be changed at any time. All three slots are enabled by
  default; unchecking one removes its slot and checking it again restores and
  repaints it.
- the tray tooltip shows the current RPM and temperature.

It has no GTK or Qt dependency. The only link dependencies are Xlib and libm.
If no hwmon value is available, the icon remains visible and the tooltip says
that the sensor is unavailable.

## Build and run

```sh
make
./traystats
```

The process must run inside an X11 session with `DISPLAY` set. On systems
where the sensor files are readable, no elevated privileges are required.
