# REFramework (Apple Silicon Edition) [![Build status](https://github.com/Ghostkwebb/REFramework-appleSilicon/actions/workflows/dev-release.yml/badge.svg)](https://github.com/Ghostkwebb/REFramework-appleSilicon/releases)

A mod framework, scripting platform, and modding tool for RE Engine games running on **Apple Silicon (M-series)** and macOS via **CrossOver**, **NotProton**, **Whisky**, or Wine/GPTK (D3DMetal / DXMT).

Based on and upstream-compatible with [praydog's REFramework](https://github.com/praydog/REFramework).

---

## Installation & Setup

Download the latest build from the [Releases](https://github.com/Ghostkwebb/REFramework-appleSilicon/releases) page (or compile using the GitHub Actions workflow).

Extract `dinput8.dll` from the downloaded archive into your game directory (the folder containing the main `.exe`, e.g. `PRAGMATA.exe`, `re2.exe`, `re4.exe`).

### 1. CrossOver (macOS)
To ensure CrossOver loads the custom `dinput8.dll` instead of Wine's internal dummy library:

1. Copy `dinput8.dll` into your game installation directory.
2. Open **CrossOver** and select the Bottle where your game or Steam is installed.
3. In the right sidebar under *Control Panels*, click **Wine Configuration** (`winecfg`).
4. Switch to the **Libraries** tab.
5. In the **New override for library** field, type `dinput8` and click **Add**.
6. Verify that `dinput8 (native, builtin)` appears in the *Existing overrides* list.
7. Click **Apply**, then **OK**.
8. Launch your game!

> **Tip**: Alternatively, you can add `WINEDLLOVERRIDES="dinput8=n,b"` to your launch environment.

---

### 2. NotProton (Steam on macOS)
If you run Windows Steam games natively on macOS using the **NotProton** compatibility tool (`CrossOver Preview` runner):

1. Extract `dinput8.dll` into your game directory.
2. In Steam (macOS), right-click your game $\to$ **Properties...** $\to$ **General**.
3. Under **Launch Options**, enter:
   ```sh
   WINEDLLOVERRIDES="dinput8=n,b" %command%
   ```
4. Launch the game from Steam. NotProton will automatically export the DLL override into the Wine environment (just like Proton on Linux).

---

### 3. Whisky / Apple Game Porting Toolkit (GPTK)
1. Copy `dinput8.dll` into your game directory.
2. Open **Whisky** and select your bottle.
3. Go to **Bottle Configuration** $\to$ **Wine Configuration** $\to$ **Libraries** tab.
4. Add a new override for `dinput8` set to **Native, Builtin** (`n,b`), then click **Apply**.
5. Launch the game.

---

### 4. Linux / Steam Deck (Proton)
1. Extract `dinput8.dll` into your game directory.
2. In Steam, right-click the game $\to$ **Properties...** $\to$ **General** $\to$ **Launch Options**:
   ```sh
   WINEDLLOVERRIDES="dinput8.dll=n,b" %command%
   ```

---

### 5. Windows (Native)
* Extract `dinput8.dll` into your game directory.
* For VR: Install SteamVR/OpenXR and extract the full zip archive into the game folder. ([VR Troubleshooting/FAQ](https://github.com/praydog/REFramework/wiki/VR-Troubleshooting)).

---

## In-Game Usage

* Press <kbd>Insert</kbd> or <kbd>F10</kbd> to open/close the REFramework in-game overlay menu.
* **Ultrawide & 16:10 Options**: Under the **Graphics** section:
  * Enable **Ultrawide/FOV/Aspect Ratio Fix**.
  * On 16:10 displays (MacBook Pro/Air), the game automatically renders at native `Uniform16x10` with 1:1 pixel-perfect mouse alignment.
  * If you prefer classic 16:9 framing with top and bottom letterboxing, check **16:10 Mode: Use Black Bars (maintain 16:9)**.

---

## Included Mods
* **Lua Scripting API & Plugin System** (All games, check out the [Wiki](https://refdocs.praydog.com))
* **VR Support**
  * Generic 6DOF VR support for all games
  * Motion controls for RE2/RE3/RE7/RE8
* **First Person** (RE2, RE3)
* **Manual Flashlight** (RE2, RE3, RE8)
* **Free Camera** (All games)
* **Scene Timescale** (All games)
* **FOV Slider** (All games)
* **Vignette Disabler** (All games)
* **Ultrawide/Aspect Ratio fixes** (All games)
* **GUI Hider/Disabler** (All games)

## Included Fixes
* **Apple Silicon / Wine / D3DMetal**:
  * 16:10 MacBook Retina display detection and viewport centering
  * 1:1 Mouse coordinate alignment under Wine translation
  * Cocoa message pump deadlock prevention during swapchain presentation
* **Game-Specific**:
  * RE8 Startup Crash & Stutters (killing enemies, taking damage, etc...)
  * MHRise/RE8 crashes related to third party DLLs

## Included Tools (Developer Mode)
* Game Objects Display
* Object Explorer

---

## Supported Games
* Resident Evil 2
* Resident Evil 3
* Resident Evil 4
* Resident Evil 7
* Resident Evil Village
* Resident Evil Requiem
* Devil May Cry 5
* Street Fighter 6
* Monster Hunter Rise
* Monster Hunter Wilds
* Monster Hunter Stories 3
* Dragon's Dogma 2
* Dead Rising Deluxe Remaster
* PRAGMATA
* Ghosts 'n Goblins Resurrection
* Apollo Justice: Ace Attorney Trilogy
* Kunitsu-Gami: Path of the Goddess
* Onimusha 2: Samurai's Destiny
* Onimusha: Way of the Sword
* Mega Man Star Force Legacy Collection

---

## Thanks
* [praydog](https://github.com/praydog) for creating REFramework.
* [SkacikPL](https://github.com/SkacikPL) for originally creating the Manual Flashlight mod.
* [cursey](https://github.com/cursey/) for helping develop the VR component and the scripting system.
* [The Hitchhiker](https://github.com/youwereeatenbyalid/) and [alphaZomega](https://github.com/alphazolam) for the great help stress testing, creating scripts for the scripting system, and helpful suggestions.
