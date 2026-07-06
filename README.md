# HydroNode 🌿

ESP32-based hydroponic irrigation controller with a cloud dashboard hosted on Render.

## Repository structure

```
hydronode/
├── server.js              ← Node.js backend (Render)
├── package.json           ← Node dependencies
├── README.md
├── public/
│   └── index.html         ← Web dashboard (served by backend)
└── firmware/
    └── hydronode/
        └── hydronode.ino  ← ESP32 Arduino sketch
```

---

## Hardware

| Component | Details |
|---|---|
| ESP32 DevKit V1 | Main controller |
| 12V 385 water pump | Via relay |
| Relay module | Active-LOW, GPIO26 |
| TDS sensor | Analog, GPIO34 |
| Float switch | GPIO27, INPUT_PULLUP |
| Push button | GPIO25, INPUT_PULLUP |
| 3S 18650 battery pack | 11.1V nominal |
| HX-3S-01 BMS | Protection board |
| MT3608 | Boost to 12V for pump |
| LM2596 | Step-down to 5V for ESP32 |
| CN3304 charger module | 3S 2A charging |
| NPN transistor (2N2222/S8050) | TDS sensor power switching |
| 1kΩ resistor | NPN base resistor |
| Toggle switch | Master power cutoff |

### GPIO pin assignments

| GPIO | Function |
|---|---|
| 26 | Relay IN (active-LOW) |
| 27 | Float switch (INPUT_PULLUP) |
| 34 | TDS sensor AOUT (ADC) |
| 32 | TDS power (NPN base via 1kΩ) |
| 25 | Push button (INPUT_PULLUP) |

---

## Render deployment

### 1. Push this repo to GitHub

```
git init
git add .
git commit -m "initial commit"
git remote add origin https://github.com/YOUR_USERNAME/hydronode.git
git push -u origin main
```

### 2. Create a Web Service on Render

- Go to [render.com](https://render.com) → New → Web Service
- Connect your GitHub repo
- Settings:
  - **Build command:** `npm install`
  - **Start command:** `node server.js`
  - **Node version:** 18+

### 3. Add environment variable

In Render dashboard → Environment → Add variable:

```
API_KEY = your-secret-key-here
```

Use any secret string — just make sure it matches what you put in the firmware.

### 4. Note your Render URL

It will look like: `https://hydronode-xxxx.onrender.com`

---

## Firmware setup

### 1. Install libraries (Arduino Library Manager)

- **ArduinoJson** by Benoit Blanchon (v6.x)
- **Preferences** (built-in ESP32 core)

### 2. Edit these values in hydronode.ino

```cpp
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* RENDER_URL    = "https://your-app-name.onrender.com";
const char* API_KEY       = "your-secret-key-here";
```

### 3. Float switch type

Default firmware assumes **Normally Closed (NC)** float switch:
```cpp
waterLow = (digitalRead(FLOAT_PIN) == HIGH);
```

If your float switch is **Normally Open (NO)**, flip to:
```cpp
waterLow = (digitalRead(FLOAT_PIN) == LOW);
```

### 4. Flash to ESP32

- Board: ESP32 Dev Module
- Upload speed: 115200
- Open Serial Monitor at 115200 baud to see push/poll logs

---

## How it works

```
ESP32  →  POST /api/update every 5s  →  Render (stores state)
ESP32  →  GET  /api/command every 3s →  Render (gets pending commands)

Browser → GET  /api/data every 2s   →  Render (live dashboard)
Browser → POST /api/mode            →  Render (queues command for ESP32)
Browser → POST /api/timer           →  Render (queues command for ESP32)
```

### Push button (physical)
Hold for 800ms → runs pump for 5 seconds (safety: disabled if water is low)

### TDS calibration
Edit `TDS_COEFF` in the firmware:
```cpp
// newCoeff = (knownPPM / sensorPPM) * 0.5
#define TDS_COEFF 0.5f
```

### Pump AUTO mode cycle
Default: 15s ON → 60s OFF. Configurable from dashboard without reflashing.
Settings survive reboot (stored in ESP32 NVS flash).

---

## Notes

- Render free tier spins down after 15 min of inactivity — first dashboard load may be slow
- ESP32 shows as OFFLINE on dashboard if no push received in last 15 seconds
- All pump modes and timer settings persist on the ESP32 across power cycles
