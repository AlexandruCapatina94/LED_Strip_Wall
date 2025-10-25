# LED_Strip_Wall

Firmware scaffold for an ESP32-C3 controlled FW1903P (WS2811-compatible) LED wall with 18 vertical strips. Each addressable "zone" is a hardware-enforced block of 14 LEDs, giving 543 zones (7 602 LEDs) overall. The firmware exposes a serial console to select effects, adjust global color, brightness, and animation speed.

## Hardware assumptions

- **Controller**: ESP32-C3 with a single FW1903P-compatible data channel on GPIO2 (5 V level shifting recommended).
- **Strips**: 18 vertical runs arranged left-to-right as eight 0.5 m strips, three 1.2 m strips, and seven 1.5 m strips. With 30 zones per meter, this yields lengths of 15, 36, and 45 logical pixels respectively.
- **Orientation**: Power/data wiring snakes up the first strip, down the second, and so on. The firmware accounts for this by marking every other strip as reversed.
- **Power**: FW1903P strips draw up to 14 W/m. With 7 602 LEDs the theoretical peak exceeds 440 W, so distribute 24 V power injection at five points and budget wiring/fusing accordingly.

## Building and uploading

The project is organized as a PlatformIO environment:

```bash
cd firmware
pio run            # build
pio run -t upload  # flash (adjust upload_port as needed)
pio device monitor # open 115200 baud serial console
```

Dependencies are declared in `platformio.ini`; PlatformIO will fetch FastLED automatically.

## Serial console commands

After flashing, open a serial terminal at 115200 baud. The firmware accepts the following commands:

- `effect <solid|rain|snake>` – select an animation.
- `color <r> <g> <b>` – set the master color (0–255 per channel).
- `brightness <0-255>` – adjust FastLED global brightness.
- `speed <multiplier>` – scale the animation speed (e.g., `0.5`, `1.0`, `2.0`).
- `status` – print current parameters and overall LED/zone counts.
- `help` – show the available commands.

## Next steps

- Add effect parameterization per strip (e.g., staggered rain start offsets).
- Persist default settings and strip descriptors to NVS/SPIFFS.
- Integrate physical rotary encoders and an OLED UI once terminal control is validated.
