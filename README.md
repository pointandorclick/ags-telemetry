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

## Installation

1. Copy `Telemetry.ash` and `Telemetry.asc` to your AGS game project folder
2. In AGS Editor, right-click on "Scripts" in the Project Explorer
3. Select "Import script..." and import both files
4. Ensure the Telemetry module appears **above** your GlobalScript in the script order

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
