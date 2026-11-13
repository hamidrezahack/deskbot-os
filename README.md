

# DeskBot OS

A desk assistant built on an **ESP8266 (NodeMCU)** with a 128x64 OLED, three buttons, a motion sensor and a sound sensor. It shows the time and weather, tracks habits and focus sessions, visualizes your life in weeks, controls a Windows laptop, and has a web dashboard with a JSON API.

> Status: **v4.0** (baseline). See [docs/ROADMAP.md](docs/ROADMAP.md) for what is planned next.

<!-- Add a photo or GIF of the device here: docs/screenshots/device.jpg -->

## Features

| Area | What it does |
|---|---|
| Clock suite | World clock with 4 faces, Pomodoro, stopwatch with laps, countdown timer, alarm |
| Weather | Live OpenWeather data for 6 cities, Celsius or Fahrenheit |
| Habits | 5 daily habits with streaks |
| Life Clock | Your life in weeks: age, weeks lived and left, life calendar, milestones |
| PC Remote | Media, volume, mute, lock and sleep for a Windows laptop through a small Python helper |
| Presence and sound | PIR desk presence and desk time, noise level and double-clap snooze |
| Alarm | Rings until dismissed or snoozed |
| WiFi manager | 3 saved networks, setup hotspot, scan and connect from a phone, no code changes |
| Web dashboard | Remote control, notifications to the OLED, token-protected JSON API |
| Updates | Over-the-air firmware updates |
| Storage | Settings, habits, Life Clock and WiFi saved in EEPROM with version and checksum |

## Hardware

| Part | Pin |
|---|---|
| SH1106 OLED 128x64 (I2C) | SDA / SCL (D2 / D1) |
| Button UP | D5 |
| Button DOWN | D6 |
| Button SELECT | D7 |
| Buzzer | D8 |
| PIR motion sensor | D0 |
| Sound sensor (HW-484) | A0 |

Controls: UP and DOWN move, SELECT confirms, UP+DOWN together goes back, long SELECT toggles the Pomodoro.

## Quick start

1. Install the **ESP8266 board package (core 3.1.2)** in the Arduino IDE and the libraries: Adafruit GFX, Adafruit SH110X, NTPClient, ArduinoJson.
2. Copy `firmware/DeskBot/secrets.example.h` to `firmware/DeskBot/secrets.h` and fill in your own values. **Never commit `secrets.h`.**
3. Open `firmware/DeskBot/DeskBot.ino`, select a NodeMCU board and upload.
4. If DeskBot cannot join your WiFi it opens a hotspot called `DeskBot-Setup`. The password is shown on the OLED. Join it and open `192.168.4.1`.
5. Open the DeskBot IP address in a browser for the dashboard. Enter the token from `secrets.h` when asked.

### PC Remote (Windows)

1. Put `companion/deskbot_pc.py` and `companion/start_deskbot_pc.bat` in one folder on the laptop.
2. Set the same token in the `.bat` file and in `secrets.h` (`PC_TOKEN`), add `PC_HOST` with the laptop IP, and upload.
3. Run the `.bat` file and use **PC Remote** from the DeskBot drawer.

## Web pages and API

| Address | Purpose |
|---|---|
| `/` | Dashboard |
| `/life` | Life Clock |
| `/wifi` | WiFi manager |
| `/api/status`, `/api/cmd`, `/api/notif`, `/api/habit`, `/api/alarm`, `/api/city` | Control and data (token required) |
| `/api/life/*`, `/api/wifi/*` | Life Clock and WiFi data (token required) |

Send the token as the `X-Token` header or `?token=`. Five wrong tries lock the sender out for 30 seconds. Keep the device on your home network only.

## Repository layout

```
firmware/DeskBot/   Arduino sketch (DeskBot.ino, LifeClock.h/.cpp, secrets.example.h)
companion/          PC helper (becomes the desktop Companion app in v5.0)
tools/              PC-side simulator and tests
docs/               Roadmap, report, screenshots
```

## Testing

The Life Clock date logic is plain C++ and was compared with an independent Python implementation (29,501 cases, no differences). OLED screens are rendered on a PC to check layout before they go on the device.

## Known limits

- The OLED font is ASCII only; non-English WiFi names show as `?`.
- Time zone is a fixed UTC offset (no automatic daylight saving).
- The PC helper is for Windows.

## Security

Credentials live in `secrets.h` (ignored by Git). The API needs a token and is meant for a trusted home network. Do not expose the device or the PC helper to the internet.

## License

To be chosen.
