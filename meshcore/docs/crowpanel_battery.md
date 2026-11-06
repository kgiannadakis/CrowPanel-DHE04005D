# CrowPanel battery indicator

The same battery icon and percentage are displayed in the headers of Chats,
Repeaters, Maps, Web Apps and Settings. Their fixed position leaves room for
the Chats back button and centered title. Lazily created headers inherit the
last displayed reading immediately; deleting a screen unregisters its widgets.
Widget creation and refresh run under the LVGL lock. STC8 I2C polling remains
outside that lock and runs at most once every five seconds, including while
the screen is dark. Unchanged values do not cause widget redraws.

The original millivolt-to-percentage calibration is unchanged. The estimate
now uses a rolling three-sample median and a low-pass filter. It waits 15
seconds after display-load or charging-state changes, then collects stable
samples again. This avoids feeding screen-off voltage recovery back into the
wake estimate. Updates are limited to two percentage points per poll, with a
one-point downward deadband except at empty. The percentage cannot increase
while discharging, or while the display is dark; actual charging increases
resume after wake settling. Sustained discharge still lowers the estimate
during long screen-off periods. Invalid voltage/state reads leave it unchanged.

This is a voltage-based estimate, not a calibrated coulomb-counting fuel gauge.
The physical battery voltage will still change with load, temperature and the
cell/pack. No NVS writes or battery persistence across reboot are introduced.

## Hardware checks

- Switch through all five tabs, including opening Maps for the first time.
- Check the icon/100% label clears the back arrow and a long device name.
- Sleep/wake repeatedly while discharging: the percentage should not jump up.
- Charge with the screen dark, then wake: the estimate should rise gradually
  after settling, rather than jumping on the first resting-voltage reading.
- Leave it dark on battery for an extended period: the estimate should still
  reflect sustained discharge after waking.

CPU light sleep, display timings and continuous boosted LoRa RX are unchanged.
