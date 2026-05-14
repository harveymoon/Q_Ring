# 🌐 Q-Ring

A pocket-sized creative coding canvas — a **240×240 round LCD** that runs your
own p5-flavored JavaScript sketches, live-reloads from your browser, and falls
back to a friendly animated face when nothing else is loaded. ✨

Built on a Waveshare RP2040-LCD-1.28 (Raspberry Pi RP2040 + GC9A01 round
display + QMI8658 IMU). USB-powered, no batteries, no cloud.

> _Plug it in, open a tab, write code, watch the screen react to gravity._  💙

---

## 🚀 Companion app

Plug your Q-Ring into a **Chrome or Edge** browser and open:

### **<https://harveymoon.github.io/Q_Ring/>**

<a href="https://harveymoon.github.io/Q_Ring/">
  <img src="docs/qr.png" alt="QR code for the companion URL" width="180">
</a>

Click *Connect Q-Ring…*, grant USB permission once, and the page will silently
auto-reconnect on every future visit. From there you can:

- 🎨 Pick **example sketches** from the dropdown and push them to the device.
- 📂 Open a **`.js` file from your computer** — it auto-pushes whenever you
  save.
- 🖼️ See a **live preview** at the same 240×240 resolution as the device.
- 🌈 Set a **theme color** (also exposed to sketches as
  `themeR` / `themeG` / `themeB`).
- 😊 Toggle between **sketch mode** and the built-in **animated face**.

The page is a single self-contained `docs/index.html` served via GitHub Pages —
no clone, no install, no CLI required.

> 💡 Use Chrome's address-bar menu → *Install Q-Ring companion…* to pin the
> page to your dock/start menu as a standalone app.

---

## 📁 What's on the device

The firmware exposes the device as a **CDC serial port** at 115200 baud. There
is no mass-storage drive — sketches are pushed over the serial link via the
companion (or the dev CLI below). Internally, the device keeps a 1 MB LittleFS
partition:

```
/sketches/current.js          ← the sketch the device runs on boot
/sketches/examples/*.js       ← built-in examples
/sketches/.settings           ← brightness, accent color, face state
/sketches/.errors/last.log    ← last sketch crash for postmortem
```

If `current.js` is missing or corrupt, the device seeds a built-in starter
sketch and falls back to the animated face. **It can't get bricked from a bad
sketch.** ✅ Errors flash a red overlay for 3 s, then the face takes over.

---

## ✏️ Writing sketches

The supported p5 surface and dialect quirks (mJS — no arrow functions, no
template strings, no `class`, etc.) are documented in
**[`data/sketches/SUPPORTED.md`](data/sketches/SUPPORTED.md)**. There's a
scrollable cheat sheet inside the companion's *Help* tab too.

The fastest loop:

1. Open the [companion](https://harveymoon.github.io/Q_Ring/) → *Pick sketch
   file…* → choose any `.js` on disk.
2. Save the file in your editor.
3. The page detects the save (~500 ms) and pushes the new bytes over USB. The
   device reloads instantly.

For folder-based workflows there's also a small **dev-mode watcher** that does
the same thing from a terminal — handy for batch examples or pairing with
build tools:

```
cd tools/qring-watch
npm install
node index.js ~/my-sketches/
```

Optional — the companion alone is usually enough.

---

## 🔌 Serial commands

Anything you can do from the companion you can also do over the raw CDC port
(115200 baud) — useful for scripting, automation, and debugging. Settings that
end with *(saved)* persist to `/sketches/.settings` across reboots.

### Sketch control

| Command | What it does |
|---|---|
| `sketch on` | Run the JS sketch. |
| `sketch off` | Halt the sketch, show the animated face. |
| `sketch reload` | Re-read `/sketches/current.js` from flash. |
| `sketch info` | Print one line: `active=N  error=N  crc=0x…  heap_free=N`. |
| `sketch list` | Enumerate sketches in `/sketches/examples/`. |
| `sketch use <name>` | Copy an example over `current.js` and reload (e.g. `sketch use clock.js`). |
| `eject` | Alias for `sketch reload` (kept from the MSC era). |

### Display & face

| Command | What it does |
|---|---|
| `bright <0-100>` | Backlight level *(saved)*. |
| `color <RRGGBB>` | Accent color *(saved)*. Also exposed to sketches as `themeR` / `themeG` / `themeB`. |
| `alert <message>` | Transient overlay (default 5000 ms; append `\|<ms>` for a custom duration, e.g. `alert hi\|2000`). |
| `<face state>` | Force the fallback face *(saved)*: `happy`, `sad`, `thinking`, `listening`, `surprised`, `sleeping`, `speaking`, `angry`, `idle`, `calibrate`. |

> Canvas rotation is sketch-controlled — call `autoRotate()` /
> `noAutoRotate()` from inside `setup()`. The fallback face always
> auto-rotates regardless.

### Filesystem

| Command | What it does |
|---|---|
| `clean` | Remove macOS dotfile spam (`.DS_Store`, etc.) from `/sketches/`. |

### Upload protocol (developer)

```
UPLOAD <path> <len>\n
<len raw bytes>
\nEND\n
```

Path is relative to LittleFS root (typical target: `/sketches/current.js`).
When the upload target matches the active sketch, the device auto-reloads.

---

## 🛠 Build & flash

```
pio run                    # compile firmware
pio run -t upload          # flash via picotool (BOOTSEL or 1200 bps reset)
pio run -t buildfs         # build the LittleFS image from data/
pio run -t uploadfs        # flash the filesystem (default sketches + docs)
```

If `picotool` can't grab the device on Windows, copy the UF2 manually:

1. Hold the **BOOTSEL** button while plugging in USB → the Pico mounts as
   `RPI-RP2`.
2. Copy `.pio/build/q_ring/firmware.uf2` onto that drive. The device reboots
   into the new firmware.

---

## 📐 Repo layout

```
src/                       firmware (C++, RP2040 / Arduino-Pico)
  main.cpp                   boot, serial dispatch, UPLOAD protocol
  face.cpp                   fallback animated face + auto-rotate
  sketch_loader.cpp          LittleFS layout, default seeding, reload
  sketch_vm.cpp              mJS interpreter wrapper, error overlay
  p5_bindings.cpp            p5-style FFI: shapes, transforms, text, IMU, theme
  imu.cpp                    QMI8658 IMU read + smoothing
  settings.cpp               brightness / color / face-state persistence
  vendor/mjs/                pinned mJS interpreter (see "Vendored" below)

data/sketches/             files flashed into LittleFS
  current.js                 active sketch (seeded if missing)
  examples/                  birthday, clock, rain, sensors, shapes, tilt, wave
  SUPPORTED.md               p5 surface + dialect notes

docs/                      GitHub Pages: companion app, PWA manifest, icon, QR
tools/qring-watch/         optional Node CLI: watch a folder, push to device
```

---

## 📦 Vendored

The mJS interpreter lives at `src/vendor/mjs/` — fetched once from
<https://github.com/cesanta/mjs>. If you wipe the tree, restore with:

```powershell
$base = 'https://raw.githubusercontent.com/cesanta/mjs/master'
Invoke-WebRequest "$base/mjs.h" -OutFile src/vendor/mjs/mjs.h
Invoke-WebRequest "$base/mjs.c" -OutFile src/vendor/mjs/mjs.c
```

---

Made with 💙 for someone who teaches the world to code.
