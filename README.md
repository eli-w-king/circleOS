# circleOS voice prototype

This first milestone connects a browser microphone and speaker to OpenAI
GPT-Live through a local Python server. The API key remains on the server.

## Requirements

- Python 3.9 or newer
- An OpenAI project API key with access to `gpt-live-1`
- Chrome, Edge, or Safari with microphone permission

## Setup

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
```

Copy `.env.example` to `.env`, then put the API key after the equals sign in
`.env`. The file is ignored by Git:

```dotenv
OPENAI_API_KEY=your-project-api-key
```

Start the server:

```sh
.venv/bin/python server.py
```

Open <http://localhost:3000>, select **Start conversation**, allow microphone
access, and begin speaking. Select **End** to close the session cleanly and
record final usage.

## Scope

This milestone uses WebRTC between the browser and GPT-Live. The next milestone
will add the local WebSocket audio gateway used by the ESP32-S3 device.

## ESP32 microphone milestone

The firmware under `firmware/` shows a minimal circleOS connection screen,
captures mono PCM16 audio at 16 kHz, streams 20 ms binary frames to
`device_gateway.py`, and plays returned PCM16 audio through its ES8311 speaker.

1. Put the Wi-Fi name and password in
   `firmware/main/device_config.h`. This ignored file must remain local.
2. Start the gateway:

   ```sh
   .venv/bin/python device_gateway.py
   ```

   Open <http://localhost:8788> to see the device connection, live microphone
   level, received sample rate, and total audio received. Tap the orb on the
   device to start or stop a billable GPT-Live session. The dashboard controls
   remain available as a fallback.

3. Activate the local ESP-IDF toolchain and build:

   ```sh
   export IDF_TOOLS_PATH="$PWD/.tools/espressif"
   source .tools/esp-idf/export.sh
   cd firmware
   idf.py build
   idf.py -p /dev/cu.usbmodem21201 flash monitor
   ```

The gateway prints microphone RMS level, peak level, and received sample rate
once per second. A stable rate near 16,000 samples/second confirms the first
device audio milestone. The device screen shows **Listening** while its gateway
connection is ready. GPT-Live uses matching mono PCM16 audio at 16 kHz in both
directions. Speaker audio is held in a bounded 240 ms queue so network jitter
cannot create unbounded playback delay.

Speaker volume remains fixed at 100%. Playback uses a saturating 2x PCM boost
so the small built-in speaker remains audible without integer wraparound on
loud peaks.

The display is intentionally text-free during normal operation. A single
batched LVGL renderer keeps voice states in related globe geometries. Its
30%-larger particle field shows a calm
rotating globe while idle, an audio-reactive globe while listening,
audio-reactive Solving while speaking, Searching while connecting or
processing, and Breathing only after a failure. The same 240 dots move to
nearest positions in the next shape with eased trajectories, so state changes
rearrange the particles rather than crossfading separate animations. An L8
canvas batches the entire swarm into one LVGL update per frame.

The device boots into a native app launcher whose icons rotate on a
depth-shaded Fibonacci sphere. The gear icon opens Quick Settings with live
Wi-Fi and battery status plus a tap-to-toggle display brightness control.
The menu is informational for Wi-Fi; network credentials remain firmware-local. The audio icon opens the voice orb; the other
active app icons are larger and softly illuminated. The microphone icon opens
the voice orb and the keyboard icon opens a local rotary keyboard; remaining
icons are inert placeholders. Launcher entries are declared in `main.c` as
icon/callback/context descriptors so additional native apps can replace
placeholders without changing the sphere renderer. Inside Voice, tap the
bottom edge to reveal a small close button, then tap it to stop any requested
session and return Home.

The rotary keyboard behaves like a safe dial. Its brighter 29-item ring tracks
finger angle directly, then continues an inertial spin and snaps to the nearest
slot after release. It starts with lowercase letters plus CAP, 123, and
backspace. CAP toggles persistent letter case; 123 switches the ring to digits
and common symbols and becomes ABC. Tapping any ring item activates it directly,
while tapping the enlarged center target activates the item at the fixed top
selection point. The next action remains as a dim ghost, and horizontal center
swipes review overflow text without committing. Keyboard-only 10 ms touch
sampling and 20 ms display refresh leave Home and Voice on their stable global
cadence. The shared bottom-edge close control returns the keyboard to Home.

A short press of the physical PWR button performs an AXP2101 hardware shutdown.
The firmware closes the active app, blanks the AMOLED, and mutes and clears
speaker playback before asking the PMU to cut power. Another PWR press performs
a clean cold boot. This is intentionally slower than soft sleep but avoids
partially initialized display, audio, or network state after waking.

A battery indicator sits at the top of the screen and stays visible across Home,
Voice, and the keyboard. The same AXP2101 poll that reads the PWR button also
reads the fuel gauge every 5 seconds, so the indicator adds no extra I2C device
or task. It shows a charge bolt in green while charging, amber at 20% or less,
red at 10% or less, and a USB glyph when the board runs without a battery. The
indicator is created and polled before Wi-Fi starts, so charge state stays
readable even while the device is retrying a connection. If all retries fail,
the firmware remains usable offline instead of aborting into a reboot loop. The
indicator hides during hardware shutdown so the AMOLED stays fully dark.
