# AGS Telemetry Module

A local telemetry logging module for [Adventure Game Studio](https://www.adventuregamestudio.co.uk/) (AGS 3.6+) games. Designed for alpha/beta testing, it tracks player sessions, interactions, idle time, room transitions, save/load events, bug reports, milestones, and errors -- all written to a plain-text log file on the player's machine.

## Features

- Session start/end with active vs idle time tracking
- Inventory-on-world and inventory-on-inventory interaction logging
- Character interaction logging (look, interact, talk, use inventory)
- Room enter/leave tracking (before and after fade)
- Save/restore slot tracking
- Unhandled interaction detection (catch-all / default response logging)
- Custom event and milestone logging
- Bug report system with automatic screenshot capture
- Build version and runtime metadata
- Optional platform tagging for multi-platform builds
- Zero dependencies -- pure AGS script module

## Installation

1. Copy `Telemetry.ash` and `Telemetry.asc` into your AGS project folder.
2. In the AGS Editor, add both files as a Script Module (right-click Scripts in the project tree > "New Script Module", then replace the generated files, or import them).
3. Make sure the Telemetry module is loaded **after** any Config module that defines `#define` constants, and **before** your GlobalScript.

Your script load order should look something like:

```
Config          (optional - for #defines like TELEMETRY_ENABLED)
Telemetry       <-- this module
[other modules]
GlobalScript
```

## Configuration

### Option A: Use the built-in defaults

The module ships with a default `TelemetryConfig_Init()` that sets sensible defaults:

| Setting | Default | Description |
|---|---|---|
| `Telemetry_IdleSecondsThreshold` | `60` | Seconds of no input before the player is considered idle |
| `Telemetry_LogPath` | `$SAVEGAMEDIR$/telemetry/telemetry.log` | Where the log file is written |
| `Telemetry_BuildVersion` | `""` | Your game version string (e.g. `"1.0.0-beta"`) |
| `Telemetry_PlatformTag` | `""` | Optional label: `"windows"`, `"mac"`, `"linux"`, `"steam"`, etc. |

### Option B: Override in your game

Implement your own `TelemetryConfig_Init()` in GlobalScript.asc (or another module loaded after Telemetry). Because AGS uses last-defined-wins for exported functions, your version will override the module's default:

```ags
// GlobalScript.asc (or a Config module loaded after Telemetry)
void TelemetryConfig_Init()
{
  Telemetry_IdleSecondsThreshold = 40;
  Telemetry_LogPath = "$SAVEGAMEDIR$/telemetry/telemetry.log";
  Telemetry_BuildVersion = "1.2.0-beta";
  Telemetry_PlatformTag = "windows";
}
```

### Optional: Conditional compilation

If you want to completely strip telemetry from release builds, define a flag in a Config module loaded before Telemetry:

```ags
// Config.ash
#define TELEMETRY_ENABLED
```

Then wrap all telemetry calls with `#ifdef`:

```ags
#ifdef TELEMETRY_ENABLED
  TelemetryConfig_Init();
  Telemetry_StartSession();
#endif
```

This is optional -- if you prefer, you can simply not call `Telemetry_StartSession()` and the module will remain inert (all functions check `Telemetry_SessionActive` before writing).

## Integration

### Required hooks

Add these calls to your `GlobalScript.asc`:

```ags
function game_start()
{
  // ... your normal setup ...

  TelemetryConfig_Init();
  Telemetry_StartSession();
}

function game_shutdown()
{
  Telemetry_EndSession();
}

function on_key_press(eKeyCode keycode, int mod)
{
  Telemetry_UserInput();
  // ... your key handling ...
}

function on_mouse_click(MouseButton button)
{
  Telemetry_UserInput();
  // ... your mouse handling ...
}

function repeatedly_execute_always()
{
  Telemetry_Tick();
}
```

### Room and game event tracking

```ags
function on_event(EventType event, int data)
{
  if (event == eEventEnterRoomBeforeFadein) {
    Telemetry_LogRoomEnter(data, true);
  }
  else if (event == eEventEnterRoomAfterFadein) {
    Telemetry_LogRoomEnter(data, false);
  }
  else if (event == eEventLeaveRoom) {
    Telemetry_LogRoomLeave(data, false);
  }
  else if (event == eEventLeaveRoomAfterFadeout) {
    Telemetry_LogRoomLeave(data, true);
  }
  else if (event == eEventGameSaved) {
    Telemetry_LogGameSaved(data);
  }
  else if (event == eEventRestoreGame) {
    Telemetry_LogGameRestored(data);
  }
}
```

### Unhandled interaction logging

```ags
function unhandled_event(int what, int type)
{
  Telemetry_LogUnhandled(what, type);
  // ... your default responses ...
}
```

### Inventory use on room targets

In your room click handler, log when the player uses an inventory item on something in the room, and when they interact with a character:

```ags
function handle_room_click(MouseButton button)
{
  if (button == eMouseLeft)
  {
    if (mouse.Mode == eModeUseinv && player.ActiveInventory != null)
    {
      bool usedDefault = (IsInteractionAvailable(mouse.x, mouse.y, eModeUseinv) == 0);
      Telemetry_LogInventoryUseAt(mouse.x, mouse.y, player.ActiveInventory, usedDefault);
    }
    else
    {
      LocationType loc = GetLocationType(mouse.x, mouse.y);
      if (loc == eLocationCharacter)
      {
        Character *c = Character.GetAtScreenXY(mouse.x, mouse.y);
        if (c != null)
        {
          bool usedDefault = (IsInteractionAvailable(mouse.x, mouse.y, mouse.Mode) == 0);
          Telemetry_LogCharacterInteraction(mouse.x, mouse.y, mouse.Mode, c, usedDefault);
        }
      }
    }

    // ... your normal click processing ...
  }
}
```

### Inventory use on other inventory items

```ags
function handle_inventory_click(MouseButton button)
{
  InventoryItem* item = inventory[game.inv_activated];

  if (button == eMouseLeftInv)
  {
    if (mouse.Mode == eModeUseinv)
    {
      if (item.ID != player.ActiveInventory.ID)
      {
        bool usedDefault = (item.IsInteractionAvailable(eModeUseinv) == 0);
        Telemetry_LogInventoryUseInv(player.ActiveInventory, item, usedDefault);
        item.RunInteraction(eModeUseinv);
      }
    }
  }

  // ... your normal inventory handling ...
}
```

### Custom events and milestones

Log arbitrary events from anywhere in your scripts:

```ags
Telemetry_LogEvent("PuzzleSolved", "opened_safe_with_combination");
Telemetry_LogMilestone("CompletedChapter1");
Telemetry_LogError("Dialog tree fell through without a match");
```

### Bug reporting

The module supports an in-game bug report flow with automatic screenshot capture. Call `Telemetry_PreCaptureBugScreenshot()` **before** showing your bug report GUI so the screenshot captures the game state, not the dialog:

```ags
function show_bug_report()
{
  Telemetry_PreCaptureBugScreenshot();
  // ... show your bug report GUI ...
}

function submit_bug_report(String description, bool cannotContinue)
{
  String screenshot = Telemetry_LogBugReport(description, cannotContinue);
  // screenshot contains the path to the saved .bmp file
}

function cancel_bug_report()
{
  Telemetry_DiscardPreCapturedScreenshot();
  // ... close your bug report GUI ...
}
```

## Log format

Plain text, one event per line, pipe-delimited with key=value pairs:

```
YYYY-MM-DD HH:MM:SS|event_type|key=value|key=value
```

### Example log

```
2026-02-07 14:33:02|session_start|date=2026-02-07
2026-02-07 14:33:02|build|version=1.2.0-beta
2026-02-07 14:33:02|platform|tag=windows
2026-02-07 14:33:02|runtime|info=...
2026-02-07 14:33:10|inv_use|item_id=4|item_name=Rope|target_type=hotspot|target_id=2|target_name=Well|default=0|x=121|y=88
2026-02-07 14:34:55|char_interact|mode=talk|char_id=1|char_name=Roger|default=0|x=221|y=130
2026-02-07 14:35:12|room_enter|room_id=10|phase=before_fadein
2026-02-07 14:35:28|game_saved|slot=5
2026-02-07 14:36:21|custom|event=PuzzleSolved|data=opened_safe
2026-02-07 14:37:00|milestone|name=CompletedChapter1
2026-02-07 14:38:15|bug_report|blocking=0|room=10|x=150|y=90|score=42|active_inv=Rope|screenshot=.../bug_1_20260207_143815.bmp|description=Door won't open
2026-02-07 14:40:01|session_end|session_seconds=419|active_seconds=300|idle_seconds=119
```

### Event types

| Event | Description |
|---|---|
| `session_start` | Game started |
| `session_end` | Game ended (includes session/active/idle seconds) |
| `build` | Build version logged at session start |
| `platform` | Platform tag logged at session start |
| `runtime` | `System.RuntimeInfo` logged at session start |
| `idle_state` | Player went idle or returned from idle |
| `inv_use` | Inventory item used on a room target |
| `inv_use_inv` | Inventory item used on another inventory item |
| `char_interact` | Character interaction (look/interact/talk/useinv) |
| `room_enter` | Player entered a room |
| `room_leave` | Player left a room |
| `game_saved` | Game saved to slot |
| `game_restored` | Game restored from slot |
| `unhandled` | Unhandled interaction (catch-all / default response) |
| `error` | Manual error report |
| `custom` | Custom event |
| `milestone` | Named milestone reached |
| `bug_report` | In-game bug report with screenshot |

## Runtime state

You can check `Telemetry_SessionActive` from any script to determine if telemetry is currently active:

```ags
if (Telemetry_SessionActive) {
  // telemetry is running
}
```

## Notes

- Logs are written to the player's save game directory by default (`$SAVEGAMEDIR$/telemetry/`). This directory is created automatically.
- Default catch-all detection relies on `IsInteractionAvailable()` and `unhandled_event`. It may not cover all edge cases for inventory-on-inventory if the engine doesn't call `unhandled_event` for those.
- Engine-level crashes or script aborts cannot be captured by script-only modules. Use `Telemetry_LogError()` for manual error reporting at known risk points.
- The `_Telemetry_WriteLine()` internal function opens and closes the file on every write. This is intentional -- it ensures data is flushed even if the game crashes.
- `System.RuntimeInfo` is logged automatically at session start, making `Telemetry_PlatformTag` optional. The tag is useful as a human-readable label when shipping multiple builds (e.g. `"steam"`, `"itch"`, `"internal"`).

## Compatibility

- AGS 3.6.0+ (tested with 3.6.2)
- Uses `File.Delete`, `File.Open` with `eFileAppend`, `SaveScreenShot`, and `System.RuntimeInfo`

## License

MIT

## Contributing

Issues and pull requests welcome at: https://github.com/pointandorclick/ags-telemetry
