# Macropad firmware

Universal ESP32-P4 UI runtime and Wi-Fi HTTP relay. The web service supplies
scenes, RGB565 artwork, state bindings and actions. New widgets composed from
supported primitives do not require new firmware. New hardware drivers or UI
primitives do require a firmware update.

## Build and browser installer

```sh
python -m pip install platformio==6.1.19
pio run -d firmware -e esp32p4_release
python firmware/scripts/package-release.py --version 0.2.0-rc1
```

Serve `dist/firmware-release` over HTTPS for ESP Web Tools. Connect the board's
USB-C UART port and close other serial applications before flashing.
The public build ignores local `secrets.h`. Wi-Fi and pairing are configured on
the device and stored in NVS. Flash ranges preserve NVS unless erase is selected.

This is a release candidate: physical display stability, relay operation with
real lamps, and browser flashing still require hardware acceptance testing.
Do not upload local credentials, device tokens, `.pio` or private app sources.
Third-party font license notices are included alongside the font sources.
