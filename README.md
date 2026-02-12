# AGS Telemetry Module

A telemetry logging module for [Adventure Game Studio](https://www.adventuregamestudio.co.uk/) (AGS) games. Track player sessions, interactions, and game events during beta testing to understand how players experience your game.

## Features

- **Session Tracking**: Log session start/end with active and idle time
- **Interaction Logging**: Track inventory use, character interactions, and unhandled actions
- **Room Tracking**: Log room enter/leave events
- **Save/Restore Tracking**: Monitor save and restore actions
- **Bug Reporting**: Capture bug reports with screenshots and game state
- **Custom Events**: Log any custom events specific to your game
- **Milestone Tracking**: Log key game milestones like act completions and endings
- **Idle Detection**: Distinguish between active play time and idle time
- **Remote Telemetry** (optional): Send events to a [telemetry dashboard](https://github.com/pointandorclick/ags-telemetry-dashboard) server in real-time via the `agsremotetelemetry` plugin, with automatic offline caching

## Installation

1. Copy `Telemetry.ash` and `Telemetry.asc` to your AGS game project folder
2. In AGS Editor, right-click on "Scripts" in the Project Explorer
3. Select "Import script..." and import both files
4. Ensure the Telemetry module appears **above** your GlobalScript in the script order
5. (Optional) Set up the [Bug Report GUI](#creating-a-bug-report-gui-ctrlr) to let players submit bug reports in-game
6. (Optional) Install the [Remote Telemetry plugin](#remote-telemetry-optional) to send events to a dashboard server in real-time

## Configuration

### Enable Telemetry and Set Your Version (Required)

Define `TELEMETRY_ENABLED` and `VERSION` in your project before importing Telemetry. This is typically done in `GlobalScript.ash` or a utility module that compiles before Telemetry:

```ags
// In GlobalScript.ash or your Util.ash
#define TELEMETRY_ENABLED
#define VERSION "1.0.0-beta"
```

`TELEMETRY_ENABLED` controls whether telemetry code is compiled into your game. Remove or comment out this line to disable telemetry entirely.

`VERSION` is automatically logged with each session and can be used elsewhere in your game (e.g., displaying version on title screen).

### Override Default Settings

Override the default configuration by implementing `TelemetryConfig_Init()` in your GlobalScript.asc:

```ags
// In GlobalScript.asc - override the default config
void TelemetryConfig_Init()
{
  Telemetry_IdleSecondsThreshold = 60;                    // Seconds before player is idle
  Telemetry_LogPath = "$SAVEGAMEDIR$/telemetry/telemetry.log";
  Telemetry_BuildVersion = VERSION;                       // Uses VERSION defined in your project
  Telemetry_PlatformTag = "windows";                      // Optional platform identifier
}
```

## Basic Usage

### Initialize and Start Session

In your `game_start()` function:

```ags
function game_start()
{
  #ifdef TELEMETRY_ENABLED
    TelemetryConfig_Init();
    Telemetry_StartSession();
  #endif

  // ... rest of your game_start code
}
```

### End Session

In your `game_shutdown()` function or `on_event`:

```ags
function game_shutdown()
{
  #ifdef TELEMETRY_ENABLED
    Telemetry_EndSession();
  #endif
}
```

### Track User Activity

In your `repeatedly_execute()` function:

```ags
function repeatedly_execute()
{
  Telemetry_Tick();  // Updates idle/active time tracking

  // ... rest of your code
}
```

In your `on_mouse_click()` and `on_key_press()` functions:

```ags
function on_mouse_click(MouseButton button)
{
  Telemetry_UserInput();  // Resets idle timer

  // ... rest of your code
}

function on_key_press(eKeyCode keycode, int mod)
{
  Telemetry_UserInput();  // Resets idle timer

  // ... rest of your code
}
```

### Track Room Changes

```ags
function on_event(EventType event, int data)
{
  if (event == eEventEnterRoomBeforeFadein) {
    Telemetry_LogRoomEnter(data, true);
  }
  else if (event == eEventLeaveRoom) {
    Telemetry_LogRoomLeave(data, false);
  }
}
```

### Track Inventory Interactions

```ags
// When player uses inventory item on something in the room
Telemetry_LogInventoryUseAt(mouse.x, mouse.y, player.ActiveInventory, usedDefaultHandler);

// When player combines two inventory items
Telemetry_LogInventoryUseInv(player.ActiveInventory, targetItem, usedDefaultHandler);
```

### Track Character Interactions

```ags
// When player interacts with a character
Telemetry_LogCharacterInteraction(mouse.x, mouse.y, mouse.Mode, targetCharacter, usedDefaultHandler);
```

### Track Save/Restore

```ags
// After saving
Telemetry_LogGameSaved(slotNumber);

// After restoring
Telemetry_LogGameRestored(slotNumber);
```

### Log Custom Events

```ags
// Log any custom event
Telemetry_LogEvent("puzzle_solved", "sliding_puzzle_room5");
Telemetry_LogEvent("achievement", "found_secret_room");
```

### Log Milestones

```ags
// Log key game progression milestones
Telemetry_LogMilestone("Start new game");
Telemetry_LogMilestone("End act 1");
Telemetry_LogMilestone("Game completed");
```

### Bug Reporting

The bug reporting feature captures a screenshot and game state when players report issues.

#### Simple Usage

```ags
// Let players report bugs with automatic screenshot
String screenshotPath = Telemetry_LogBugReport("Player description of bug", false);
```

#### Creating a Bug Report GUI (Ctrl+R)

For a better user experience, create a GUI that lets players describe bugs and indicate severity.

**Step 1: Create the GUI in AGS Editor**

1. Create a new GUI called `gBugReport`
2. Add these controls:
   - `txtBugDescription` - A TextBox for the bug description
   - `btnBugBlocking` - A Button to toggle "Can continue" / "CANNOT continue"
   - `btnBugSubmit` - A Button labeled "Submit"
   - `btnBugCancel` - A Button labeled "Cancel"

**Step 2: Link button events in the GUI**

In the AGS Editor, select the `gBugReport` GUI. For each button, click on it, go to the **Events** tab (lightning bolt icon), and link its `OnClick` event to the corresponding function in GlobalScript:
   - `btnBugBlocking` -> `btnBugBlocking_OnClick`
   - `btnBugSubmit` -> `btnBugSubmit_OnClick`
   - `btnBugCancel` -> `btnBugCancel_OnClick`

**Step 3: Add the dialog code to GlobalScript.asc**

```ags
bool _bugReportBlocking = false;

function show_bug_report_dialog()
{
  _bugReportBlocking = false;
  txtBugDescription.Text = "";
  btnBugBlocking.Text = "Can continue";
  gBugReport.Visible = true;
}

function btnBugBlocking_OnClick(GUIControl *control, MouseButton button)
{
  _bugReportBlocking = !_bugReportBlocking;
  if (_bugReportBlocking) {
    btnBugBlocking.Text = "CANNOT continue";
  } else {
    btnBugBlocking.Text = "Can continue";
  }
}

function btnBugSubmit_OnClick(GUIControl *control, MouseButton button)
{
  String desc = txtBugDescription.Text;
  if (desc == "") {
    Display("Please describe the bug.");
    return;
  }

  Telemetry_LogBugReport(desc, _bugReportBlocking);
  gBugReport.Visible = false;
  Display("Bug report submitted. Thank you!");
}

function btnBugCancel_OnClick(GUIControl *control, MouseButton button)
{
  gBugReport.Visible = false;
}
```

**Step 4: Add keyboard shortcut (Ctrl+R)**

In your `on_key_press()` function:

```ags
function on_key_press(eKeyCode keycode, int mod)
{
  Telemetry_UserInput();

  // Check for Ctrl key modifier
  if (mod & eKeyModCtrl)
  {
    if (keycode == eKeyR)
    {
      // Ctrl+R opens bug report dialog
      if (!gBugReport.Visible) {
        show_bug_report_dialog();
      }
    }
  }

  // ... rest of your key handling
}
```

**Step 5: Allow Escape to close the dialog**

```ags
// In on_key_press, handle Escape
if (keycode == eKeyEscape)
{
  if (gBugReport.Visible)
  {
    gBugReport.Visible = false;
    return;  // Don't process escape further
  }
}
```

The bug report logs:
- Timestamp
- Whether it's blocking (player cannot continue)
- Current room, player position, and score
- Active inventory item
- Screenshot filename
- Player's description

## Remote Telemetry (Optional)

Send telemetry events to a dashboard server in real-time using the `agsremotetelemetry` plugin. Events are sent over HTTP on a background thread so the game is never blocked. If the server is unreachable, events are cached to a local file and retried automatically. The cache file uses the same pipe-delimited format as `telemetry.log`, so it can also be manually imported via the dashboard.

### Plugin Installation

#### 1. Build the plugin

See [Building the Plugin](#building-the-plugin) below for prerequisites and build instructions.

#### 2. Install in the AGS Editor

Place the compiled plugin library in your AGS Editor directory (the folder containing `AGSEditor.exe`):

| Platform | File to copy | Destination |
|----------|-------------|-------------|
| Windows  | `agsremotetelemetry.dll` | Same directory as `AGSEditor.exe` |
| Linux    | `libagsremotetelemetry.so` | `Linux/lib64/` relative to the editor |
| macOS    | `libagsremotetelemetry.dylib` | Same directory as the engine binary |

Then in AGS Editor:
1. Open your game project
2. In the Project Explorer, expand the **Plugins** node
3. Right-click **AGS Remote Telemetry** and select **Use plugin**
4. Save your project

#### 3. Distribute with your game

When you build your game, the plugin must also be shipped alongside the game executable so it loads at runtime:

- **Windows**: Place `agsremotetelemetry.dll` in the same folder as your game's `.exe`
- **Linux**: Place `libagsremotetelemetry.so` in the same folder as the `ags` binary (or in a `lib/` subdirectory)
- **macOS**: Place `libagsremotetelemetry.dylib` alongside the engine in the app bundle

### Enable Remote Telemetry

Add these defines to your project (e.g., `GlobalScript.ash`) alongside the existing telemetry defines:

```ags
#define TELEMETRY_ENABLED
#define VERSION "1.0.0-TesterName"
#define TELEMETRY_SERVER_URL "http://your-dashboard-server:3000"
#define TELEMETRY_API_KEY "your-api-key"  // optional, omit if no auth configured
```

That's it. No other code changes are needed. The module automatically initializes the plugin on session start and sends events as they're logged. The local `telemetry.log` file is always written regardless of whether remote telemetry is active.

### How It Works

1. On `Telemetry_StartSession()`, the plugin connects to the dashboard and creates a remote session
2. Each event logged via the module is simultaneously written to the local file AND queued for remote sending
3. The plugin batches events and sends them on a background thread (up to 20 events per request)
4. On `Telemetry_EndSession()`, remaining events are flushed and the remote session is closed

### Offline Behavior

- If the dashboard server is unreachable, events are cached to `remote_cache.log` in the telemetry directory
- On the next session start, cached events are sent before new events
- The cache file uses the same pipe-delimited format as `telemetry.log` and can be manually imported via the dashboard's import feature
- The local `telemetry.log` is always written, so no data is ever lost

### Building the Plugin

#### Prerequisites

- **CMake** 3.14 or later
- A **C++11** compiler (GCC, Clang, or MSVC)
- **libcurl** development libraries
- **AGS Plugin SDK header** (`agsplugin.h`)

Install libcurl development packages for your platform:

```bash
# Debian/Ubuntu
sudo apt install libcurl4-openssl-dev

# macOS (Homebrew)
brew install curl

# Windows (vcpkg)
vcpkg install curl
```

#### Build Steps

```bash
cd plugin

# Download the AGS plugin SDK header
curl -O https://raw.githubusercontent.com/adventuregamestudio/ags/master/Engine/plugin/agsplugin.h

# Build
mkdir build && cd build
cmake ..
cmake --build . --config Release
```

The compiled plugin will be in the `build/` directory:
- **Windows**: `build/Release/agsremotetelemetry.dll`
- **Linux**: `build/libagsremotetelemetry.so`
- **macOS**: `build/libagsremotetelemetry.dylib`

### Disabling Remote Telemetry

Remove or comment out the `#define TELEMETRY_SERVER_URL` line. The plugin can remain loaded but will not be initialized. Local file logging continues to work normally.

## Log File Format

The telemetry log uses a pipe-delimited format:

```
2025-03-15 14:23:45|session_start|date=2025-03-15
2025-03-15 14:23:45|build|version=1.0.0-beta
2025-03-15 14:23:45|runtime|info=Adventure Game Studio run-time engine...
2025-03-15 14:23:46|room_enter|room_id=1|phase=after_fadein
2025-03-15 14:25:30|inv_use|item_id=5|item_name=Key|target_type=hotspot|target_id=3|target_name=Door|default=0|x=150|y=100
2025-03-15 14:28:00|milestone|name=End act 1
2025-03-15 14:30:00|idle_state|idle=1
2025-03-15 14:35:00|session_end|session_seconds=690|active_seconds=390|idle_seconds=300
```

## Log File Location

By default, the log file is saved to:
- **Windows**: `%USERPROFILE%\Saved Games\<GameName>\telemetry\telemetry.log`
- **macOS**: `~/Library/Application Support/<GameName>/telemetry/telemetry.log`
- **Linux**: `~/.local/share/ags/<GameName>/telemetry/telemetry.log`

## Disabling Telemetry

To completely disable telemetry at compile time, remove or comment out the `#define TELEMETRY_ENABLED` line in your `GlobalScript.ash` (or wherever you defined it). This removes all telemetry code from the compiled game.

## Runtime Check

You can check if telemetry is active at runtime:

```ags
if (Telemetry_SessionActive) {
  // Show version number or beta indicator
}
```

## License

MIT License - feel free to use in your AGS projects.

## Contributing

Issues and pull requests welcome at: https://github.com/pointandorclick/ags-telemetry
