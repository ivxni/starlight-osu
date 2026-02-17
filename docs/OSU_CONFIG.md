# starlight-osu Configuration Reference

All settings are managed through the web dashboard at `http://localhost:8000/{uuid}/osu` and synced to the C++ client via HTTP polling. A local INI fallback (`slhost_osu_config.ini`) is used when the web backend is unavailable.

---

## Relax (Humanized Auto-Click)

Automates key presses so the player only needs to aim. Uses statistical models (Gaussian hit timing, Log-Normal hold times) to produce human-like tapping patterns that are indistinguishable from a real player on replay analysis tools.

### Core Settings

| Setting | Key | Type | Default | Description |
|---|---|---|---|---|
| **Enabled** | `enabled` | bool | `true` | Master toggle for the relax module. |
| **Key 1** | `key1` | string | `Z` | First keyboard key (K1). |
| **Key 2** | `key2` | string | `X` | Second keyboard key (K2). |
| **Target Accuracy** | `targetAccuracy` | float | `0.0` | Target accuracy % (0 = disabled). When set (e.g. 98.0), the bot dynamically adjusts UR and offset to converge on this accuracy by end of map. |
| **Global Offset** | `globalOffset` | float | `0.0` | Fixed ms offset applied to all hits. Negative = early, positive = late. Compensates for audio latency. |

### Hit Timing (Gaussian Model)

Controls when each note is tapped relative to its ideal time. Uses a normal distribution -- the same shape seen in legit player hit-error histograms.

| Setting | Key | Type | Default | Description |
|---|---|---|---|---|
| **Unstable Rate** | `unstableRate` | float | `80.0` | Base UR. Standard deviation = UR / 10. Speed players: 70-90. Average players: 90-120. Below 60 is suspicious. |
| **Hit Offset Mean** | `hitOffsetMean` | float | `-2.0` | Mean hit offset in ms. Most humans hit slightly early (-2 to -8ms). Setting to 0 is suspicious. |

### Hold Time (Log-Normal Model)

Controls how long each key is held. Uses a log-normal distribution which matches real keystroke dynamics (no negative values, natural right-skew tail). Defaults are tuned for 280-300 BPM stream capability.

| Setting | Key | Type | Default | Description |
|---|---|---|---|---|
| **Hold Mean (Stream)** | `holdMeanStream` | float | `30.0` | Mean hold duration for notes in streams (gap < singletap threshold). 30ms handles 300 BPM (50ms gaps). Increase for lower BPM comfort. |
| **Hold Mean (Single)** | `holdMeanSingle` | float | `65.0` | Mean hold duration for singles/jumps (gap >= singletap threshold). |
| **Hold Variance** | `holdVariance` | float | `0.20` | Shape factor. 0.1 = very consistent, 0.5 = sloppy. 0.15-0.25 for speed players, 0.25-0.35 for casual. |

### Slider Handling

Sliders are held until near the end, with release timing drawn from a normal distribution. osu! allows releasing ~36ms early for full score.

| Setting | Key | Type | Default | Description |
|---|---|---|---|---|
| **Slider Release Mean** | `sliderReleaseMean` | float | `18.0` | Average ms before slider end to release. |
| **Slider Release Std** | `sliderReleaseStd` | float | `8.0` | Standard deviation of slider release timing. |

### Singletap vs Alternate

Human players singletap slow sections and alternate only in streams. The threshold is set low (125ms ~ 240 BPM) so the bot starts alternating early, as a speed player would.

| Setting | Key | Type | Default | Description |
|---|---|---|---|---|
| **Singletap Threshold** | `singletapThreshold` | float | `125.0` | Gap between notes in ms. If gap > threshold: singletap (same key). If gap <= threshold: alternate. 125ms = ~240 BPM boundary (speed player). Raise to ~180ms for casual play style. |

### Advanced

| Setting | Key | Type | Default | Description |
|---|---|---|---|---|
| **Min Click Gap** | `minClickGap` | float | `8.0` | Minimum ms between key release and next press. Must be very low for high BPM. 8ms handles 300+ BPM. Increase for a more relaxed style. |
| **Fatigue Factor** | `fatigueFactor` | float | `1.5` | UR increase per 100 notes. Simulates human fatigue. 1.5 = good stamina (speed player). 3-5 = gets tired fast. Set 0 to disable. |

### Tuning Guide

- **Speed player profile (default)**: UR 80, offset -2, holdMeanStream 30, minClickGap 8, singletapThreshold 125, fatigue 1.5. Handles 280-300 BPM streams comfortably.
- **Casual player profile**: UR 100-120, offset -4, holdMeanStream 50, minClickGap 15, singletapThreshold 180, fatigue 3.0.
- **Target 98% acc**: Set `targetAccuracy=98.0`. The controller will adjust UR/offset dynamically.
- **BPM math**: At 300 BPM 1/4 streams, notes are 50ms apart. Minimum cycle = holdMeanStream + minClickGap. Keep this sum below the note gap.
- **Streams**: The bot auto-detects streams vs singles via the singletap threshold.
- **Slider breaks**: Now properly calculated from beatmap timing points (BPM + SV).

---

## Aim Assist (Cursor Correction)

Applies subtle cursor correction toward upcoming hit objects. Uses the WindMouse algorithm for natural-looking mouse movement. This is a secondary/foundational feature, designed to be undetectable.

| Setting | Key | Type | Default | Range | Description |
|---|---|---|---|---|---|
| **Enabled** | `enabled` | bool | `false` | -- | Master toggle. Off by default since relax is the primary feature. |
| **Strength** | `strength` | float | `0.3` | 0.0 to 1.0 | How aggressively the cursor is corrected toward the target. `0.0` = no correction, `1.0` = full snap. Values above 0.5 are risky for detection. Displayed as percentage in the UI. |
| **Assist Radius** | `assistRadius` | float | `120.0` | 10 to 300 osu!px | Only applies correction when the cursor is within this distance (in osu! playfield pixels) of the target. Outside this radius, the assist does nothing. Larger values activate earlier but are more noticeable. |
| **Look Ahead** | `lookAheadMs` | float | `300.0` | 50 to 1000 ms | How far ahead (in audio time) to start targeting the next hit object. Higher values start correcting earlier, giving smoother movement but potentially moving before the player intends to. |
| **Look Behind** | `lookBehindMs` | float | `50.0` | 0 to 200 ms | Continue assisting for this many ms after the hit object's timing point has passed. Helps with late hits and slider starts. |
| **Smoothing** | `smoothing` | float | `4.0` | 1.0 to 20.0 | Controls how gradually the cursor correction is applied. Higher values = slower, smoother movement. Lower values = snappier correction. Acts as a divisor on the correction delta per frame. |
| **WindMouse** | `windMouseEnabled` | bool | `true` | -- | Enables the WindMouse humanization algorithm. Adds realistic micro-movements (wind and gravity forces) to cursor paths instead of moving in straight lines. Strongly recommended to keep on. |
| **Gravity (Min)** | `gravityMin` | float | `3.0` | 0.0 to 20.0 | WindMouse parameter: minimum gravitational pull toward the target. Gravity keeps the cursor trending toward the destination. |
| **Gravity (Max)** | `gravityMax` | float | `7.0` | 0.0 to 20.0 | WindMouse parameter: maximum gravitational pull. Actual gravity is randomized between min and max per movement step. |
| **Wind (Min)** | `windMin` | float | `0.5` | 0.0 to 10.0 | WindMouse parameter: minimum random wind force. Wind adds organic, unpredictable deviations to the cursor path. |
| **Wind (Max)** | `windMax` | float | `3.0` | 0.0 to 10.0 | WindMouse parameter: maximum random wind force. Higher wind = more erratic movement. Keep proportional to gravity. |
| **Damping** | `damping` | float | `0.8` | 0.0 to 1.0 | WindMouse velocity damping factor. Controls how quickly previous velocity decays. `1.0` = no damping (drifty), `0.0` = full damping (stops instantly). `0.7-0.9` feels natural. |
| **Easing** | `easingEnabled` | bool | `true` | -- | Applies smoothstep easing near the target, making the cursor decelerate as it approaches the hit circle. Prevents robotic snap-to-center movement. |

### Aim Assist Tuning Guide

- **Subtle (Recommended)**: strength 0.15-0.30, radius 80-120, smoothing 4-8. Barely noticeable but helps with aim consistency.
- **Moderate**: strength 0.3-0.5, radius 120-180, smoothing 2-4. More noticeable correction, use with caution.
- **WindMouse Balance**: Gravity should be 2-4x the wind values. If wind > gravity, the cursor wanders too much before reaching the target.

---

## API Endpoints

All endpoints are served from `http://localhost:8000`.

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/{uuid}/osu/config` | Fetch the user's osu! config. Creates defaults if none exists. |
| `PUT` | `/api/{uuid}/osu/config` | Save/update the user's osu! config. |
| `GET` | `/api/osu/defaults` | Returns default values for all osu! settings. |
| `POST` | `/api/osu/gamestate` | Push live game state from the C++ client (rate-limited to ~5 Hz). |
| `GET` | `/api/osu/gamestate` | Read the latest pushed game state. |

---

## Game State (Push)

The C++ client pushes a snapshot of the current osu! game state to the backend every ~200ms. This is used for the web dashboard to display live status.

| Field | Type | Description |
|---|---|---|
| `gameState` | int | Current game state enum: `0` = Menu, `1` = Editing, `2` = Playing, `5` = Song Select, `7` = Ranking, `11` = Multiplayer |
| `audioTime` | int | Current audio playback position in milliseconds. |
| `mods` | int | Active mod bitfield. See mod flags below. |
| `combo` | int | Current combo count. |
| `score` | int | Current score. |
| `playerHP` | float | Player health bar (0.0 to 1.0). |
| `beatmapAR` | float | Approach Rate of the current beatmap. |
| `beatmapCS` | float | Circle Size of the current beatmap. |
| `beatmapOD` | float | Overall Difficulty of the current beatmap. |
| `beatmapHP` | float | HP Drain of the current beatmap. |
| `hitObjectCount` | int | Total number of hit objects in the current beatmap. |
| `currentHitIndex` | int | Index of the next hit object to be played. |

### Mod Bitflags

| Bit | Value | Mod |
|---|---|---|
| 0 | 1 | NoFail (NF) |
| 1 | 2 | Easy (EZ) |
| 2 | 4 | TouchDevice (TD) |
| 3 | 8 | Hidden (HD) |
| 4 | 16 | HardRock (HR) |
| 5 | 32 | SuddenDeath (SD) |
| 6 | 64 | DoubleTime (DT) |
| 7 | 128 | Relax (RX) |
| 8 | 256 | HalfTime (HT) |
| 9 | 512 | Nightcore (NC) |
| 10 | 1024 | Flashlight (FL) |
| 11 | 2048 | Auto |
| 12 | 4096 | SpunOut (SO) |
| 13 | 8192 | Autopilot (AP) |
| 14 | 16384 | Perfect (PF) |

---

## INI File Fallback

When the web backend is unreachable, the C++ client reads/writes `slhost_osu_config.ini` next to the executable. Structure:

```ini
[auth]
uuid=your-uuid-here

[relax]
enabled=1
key1=A
key2=S
targetAccuracy=0.0
unstableRate=80.0
hitOffsetMean=-2.0
holdMeanStream=30.0
holdMeanSingle=65.0
holdVariance=0.20
sliderReleaseMean=18.0
sliderReleaseStd=8.0
singletapThreshold=125.0
minClickGap=8.0
globalOffset=0.0
fatigueFactor=1.5

[aim]
enabled=0
strength=0.30
assistRadius=120.0
lookAheadMs=300.0
lookBehindMs=50.0
smoothing=4.0
windMouseEnabled=1
gravityMin=3.00
gravityMax=7.00
windMin=0.50
windMax=3.00
damping=0.80
easingEnabled=1
```

The `[auth] uuid` field links the local client to the web dashboard user account. Set this to your UUID from the web login to enable config syncing.
