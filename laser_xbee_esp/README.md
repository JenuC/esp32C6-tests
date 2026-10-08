
If you do want ESP-IDF, pick one of these:

- VS Code (easiest, any OS): install the "ESP-IDF" extension by Espressif, run ESP-IDF: Configure ESP-IDF Extension, and choose version 5.5. Then open the laser_xbee_esp folder. The status bar has buttons to set the target (esp32c6), build, flash and monitor, so you never type idf.py yourself.
- Windows: use the "ESP-IDF Tools Installer" from Espressif's website. It adds an "ESP-IDF 5.5 PowerShell" shortcut, and idf.py works inside that shell.

```bash
 git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
 cd ~/esp/esp-idf && ./install.sh esp32c6,esp32c5
 . ~/esp/esp-idf/export.sh  
  
 idf.py set-target esp32c6
 idf.py build flash monitor
 gcc -Imain test/test_frame.c main/xbee_frame.c -o t && ./t
```

# laser_xbee_esp

Sends the laser status from an ESP32-C6/C5/H2 straight to the existing XBee 802.15.4 indicator, so no XBee is needed on the monitor side.

The frame matches what XB24 firmware 10EF sends with CH=15, ID=B01, MY=<id>, DL=0, MM=0:

    61 88 | seq | 01 0B | 00 00 | <id> 00 | digi | 00 cmd | FCS (added by hardware)

## Build
ESP-IDF >= 5.1 for C6/H2, >= 5.5 for C5.

    idf.py set-target esp32c6     # or esp32c5 / esp32h2
    idf.py build flash monitor

## Configure
Edit `main/config.h`: MONITOR_ID, LASER_GPIO and its polarity, RESEND_INTERVAL_MS.

## Verify before deploying
1. Set `SNIFF_MODE 1`, flash, and power the real monitor. You should see its frames,
   e.g. `61 88 46 01 0B 00 00 01 00 83 00 0B`.
2. Set `SNIFF_MODE 0`, unplug the original monitor, and toggle the laser input.
   `cmd 11 acked` in the log means the indicator's XBee accepted the frame.

Host test of the frame builder against captured packets:

    gcc -Imain test/test_frame.c main/xbee_frame.c -o t && ./t
