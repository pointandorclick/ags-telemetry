// AGS Telemetry Module - Public API
// https://github.com/pointandorclick/ags-telemetry
//
// A telemetry logging module for Adventure Game Studio (AGS) games.
// Tracks player sessions, interactions, and game events for beta testing.
//
// REQUIRED: Define VERSION in your project (e.g., in GlobalScript.ash or a Util module):
//   #define VERSION "1.0.0-beta"

// Comment out the line below to completely disable telemetry at compile time
#define TELEMETRY_ENABLED

// Configuration variables (set these in TelemetryConfig_Init)
import int Telemetry_IdleSecondsThreshold;   // Seconds before player is considered idle
import String Telemetry_LogPath;              // Path to telemetry log file
import String Telemetry_UseUntilDate;         // Expiry date "YYYY-MM-DD" or empty to disable
import String Telemetry_QuitMessage;          // Message shown when build expires
import String Telemetry_BuildVersion;         // Your game version string
import String Telemetry_PlatformTag;          // Optional platform identifier

// Core session functions
import void Telemetry_StartSession();         // Call in game_start()
import void Telemetry_EndSession();           // Call in game_shutdown() (on_event with eEventLeaveRoom also works)
import void Telemetry_Tick();                 // Call in repeatedly_execute()
import void Telemetry_UserInput();            // Call in on_mouse_click() and on_key_press()

// Interaction logging
import void Telemetry_LogInventoryUseAt(int x, int y, InventoryItem *usedItem, bool usedDefaultCatchAll);
import void Telemetry_LogInventoryUseInv(InventoryItem *usedItem, InventoryItem *targetItem, bool usedDefaultCatchAll);
import void Telemetry_LogCharacterInteraction(int x, int y, CursorMode mode, Character *target, bool usedDefaultCatchAll);

// Room tracking
import void Telemetry_LogRoomEnter(int roomId, bool beforeFadeIn);
import void Telemetry_LogRoomLeave(int roomId, bool afterFadeOut);

// Save/restore tracking
import void Telemetry_LogGameSaved(int slot);
import void Telemetry_LogGameRestored(int slot);

// Error and event logging
import void Telemetry_LogUnhandled(int what, int type);
import void Telemetry_LogError(String message);
import void Telemetry_LogEvent(String eventName, String data);
import void Telemetry_LogMilestone(String milestoneName);

// Bug reporting - returns screenshot filename (or empty string if telemetry disabled)
import String Telemetry_LogBugReport(String description, bool cannotContinue);

// Configuration initializer - implement this in your game to set config values
import void TelemetryConfig_Init();

// Runtime state - true if telemetry session is active (safe to check in room scripts)
import bool Telemetry_SessionActive;
