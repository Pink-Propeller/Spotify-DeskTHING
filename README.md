# Spotify Desk Thing

A small DIY desk controller for Spotify, built on an ESP32-S3 with a 2.8" touchscreen and a volume knob, in a 3D-printed case. It shows what's playing on your Spotify account and lets you play, pause, skip, shuffle, browse your playlists and recently played, and change the volume from your desk.

It controls whatever device Spotify is already playing on (your phone, computer or a speaker). The Desk Thing doesn't play audio itself.

> This is an unofficial fan project and is not made or supported by Spotify.

---

## What you need

**Hardware**

- **ESP32-S3 2.8" touchscreen board:** LCDWIKI ES3C28P (320×240, ILI9341 display, FT6336 touch, 16 MB flash, 8 MB PSRAM).
- **EC11 rotary encoder module:** for volume.
- **USB-C cable**
- **Fasteners:**
  - 3 × M3×16 and 1 × M3×14 screws for the case.
  - 4 × M2×6 screws for the encoder.
- **Printed parts:** front face, text inlay, back face, stand and knob (see the STL files).

**Accounts**

- **Spotify Premium.** Spotify only allows playback control with Premium.
- **A free Spotify Developer account:** [developer.spotify.com](https://developer.spotify.com/dashboard)

**Software**

- **[Arduino IDE](https://www.arduino.cc/en/software)** 2.x
- **ESP32 board package:** "esp32 by Espressif Systems", 3.x (Boards Manager)
- **Libraries** (Library Manager):
  - **LovyanGFX** by lovyan03
  - **ArduinoJson** by Benoit Blanchon, version 7.x

---

## 1. Wire the encoder

| Encoder pin | Board pin |
|---|---|
| CLK (A) | IO14 |
| DT (B) | IO21 |
| + | 3.3V |
| GND | GND |

The push switch (SW) isn't used.

**Wire straight to the encoder's own legs, not the module header.** On some modules, the resistors on the header stop the board reading the knob.

If the volume goes the wrong way when you turn the knob, change `#define ENC_REVERSE false` to `true` in the code.

---

## 2. Create your Spotify app

1. Go to the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard) and click **Create app**.
2. Give it any name and description.
3. Under **Redirect URIs**, add exactly:
   ```
   http://127.0.0.1:8888/callback
   ```
4. Tick **Web API** and save.
5. Open the app's **Settings** and copy the **Client ID** and **Client secret**.
6. If someone other than the app's owner will log in on the device, add their Spotify email under **User Management**. New apps allow up to 5 users.

---

## 3. Put your details in the code

Keep both files in a folder called `SpotifyCarThing`:

```
SpotifyCarThing/
├── SpotifyCarThing.ino
└── InterFonts.h
```

Open `SpotifyCarThing.ino` and fill in the only two lines you must change:

```cpp
#define SPOTIFY_CLIENT_ID     "your-spotify-client-id"
#define SPOTIFY_CLIENT_SECRET "your-spotify-client-secret"
```

**Optional: time zone.** The clock is set to New Zealand time. To change it, replace `TZ_INFO` with your zone's POSIX string, for example:

| Region | `TZ_INFO` |
|---|---|
| New Zealand | `NZST-12NZDT,M9.5.0,M4.1.0/3` |
| Sydney | `AEST-10AEDT,M10.1.0,M4.1.0/3` |
| London | `GMT0BST,M3.5.0/1,M10.5.0` |
| New York | `EST5EDT,M3.2.0,M11.1.0` |
| Los Angeles | `PST8PDT,M3.2.0,M11.1.0` |

You don't need to enter Wi-Fi in the code. You pick your network on the device.

---

## 4. Board settings and upload

In Arduino IDE, choose **Tools** and set:

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** |
| Flash Size | **16MB (128Mb)** |
| Partition Scheme | **16M Flash (3MB APP/9.9MB FATFS)** |
| PSRAM | **OPI PSRAM** |
| USB CDC On Boot | **Enabled** |
| CPU Frequency | 240MHz (WiFi) |
| Flash Mode | QIO 80MHz |
| Upload Speed | 921600 |
| Erase All Flash Before Sketch Upload | Disabled |

Then click **Upload**.

- **If the upload won't start:** hold **BOOT**, tap **RST**, release **BOOT**, then upload again. Press **RST** when it finishes.
- **To watch what's happening:** open the Serial Monitor at **115200** baud.

---

## 5. First start

1. **Wi-Fi.** The device looks for networks and shows a list. Tap yours, type the password on the on-screen keyboard and tap **Join**. It remembers the network from then on.
2. **Log in to Spotify.**
   1. The screen shows a QR code and an address such as `http://192.168.1.50`. You can also use **http://carthing.local**.
   2. Open that address on a phone or computer on the same Wi-Fi.
   3. On that page, tap **Log in with Spotify** and agree.
   4. Spotify then opens a page that won't load. That's expected. Copy the **whole address** from the browser's address bar.
   5. Paste it into the box on the device's page and tap **Connect**.
3. **Done.** Start playing something on any device signed in to your Spotify account, and it appears on the Desk Thing within a couple of seconds.

The login is saved, so you only do this once.

---

## Using it

**Screens**

- **Now Playing:**
  - Album art, song, artist and progress bar.
  - Shuffle, previous, play/pause, next and repeat buttons.
  - Drag the progress bar to seek.
  - Swipe left or right on the art to skip.
- **Library:**
  - **Recently played:** the last six things you listened to.
  - **Your Library:** all your playlists, with Liked Songs at the top.
  - The mini player at the bottom takes you back to Now Playing.
- **Playlist:** tap any song to play it, or use the green play and shuffle buttons.
- **Standby:** a large clock with the date and current song. Tap anywhere to wake it.

**Top bar**

- **Library icon or back arrow:** go to the Library, or back.
- **Clock:** tap to switch between 12-hour and 24-hour time.
- **Wi-Fi icon:** tap to change network.
- **Moon:** go to Standby now.

**Gestures**

- **Swipe down from the top edge:** Settings.
- **Swipe up from the bottom edge:** Account info.
- **Swipe in from the left edge:** back.

**Knob**

- Turn it to change the volume. A volume bar drops down from the top: green up to 60%, orange up to 80%, red above that.

**Settings**

- **Theme:** dark or light.
- **Brightness:** slider.
- **Sleep:** 1, 5, 10 or 20 minutes, or never.
- **Screensaver:** on or off, with four styles:
  - Standby
  - White
  - Green
  - Pink Propeller
- **Wi-Fi:** change network.
- **Log out:** tap twice to confirm.

**Sleep**

- With the screensaver **on**, the device shows the screensaver after the sleep time.
- With it **off**, the screen turns off.
- A tap, a turn of the knob, or a song starting on Spotify wakes it.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Screen stays black after upload | Check **PSRAM = OPI PSRAM** and the partition scheme. The Serial Monitor shows `PSRAM allocation FAILED` if PSRAM is off. |
| Screen shows stripes, glitches or wrong colours | Change `#define SPI_WRITE_MHZ 80` to `40` and upload again. |
| "Spotify only shares songs from playlists you made or collaborate on" | Spotify only lets apps like this one read songs from playlists you own or collaborate on. Playlists you just follow show this message. |
| "Spotify Premium is needed for playback control" | Playback control needs Premium. |
| "No Spotify device found" | Open Spotify on your phone or computer and play something once. The Desk Thing controls an existing Spotify device. |
| Recently played asks for permission | Log out (in Settings) and log in again, so the new permission is granted. |
| Nothing shows up after logging in | Check the Client ID and secret, that the Redirect URI matches exactly, and that your account is listed under **User Management** if you're not the app owner. |
| Knob does nothing | Check it's wired to IO14/IO21 on the encoder's legs. Watch the Serial Monitor for `[enc]` lines while turning it. |
| "Reconnecting…" in the top bar | Wi-Fi or Spotify is briefly unreachable. It recovers by itself. |
| Can't open carthing.local | Use the IP address shown on the device's screen instead. Some networks block `.local` names. |

**Starting fresh:** to wipe the saved Wi-Fi, login and settings, set **Erase All Flash Before Sketch Upload** to **Enabled** for one upload, then set it back.

---

## Credits

Designed and built by **Pink Propeller**. Interface text uses the [Inter](https://rsms.me/inter/) typeface (SIL Open Font License).
