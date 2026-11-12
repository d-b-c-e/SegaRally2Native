// The Wheelkit runtime (wheelkit.dll, loaded beside the program when it is
// there): the wheel profile's input into the cabinet's controls, telemetry
// from the game's RAM and a log of the drive board's motor commands.
// NOT part of upstream SegaRally2Native (snicolast): a modified version's
// addition. Built only with -DWHEELKIT_SDK=<wheelkit checkout>; without it, or
// without the DLL, the game is upstream's.
#pragma once

struct Inputs;

#ifdef SR2_WHEELKIT
// Loads and starts the runtime (no window yet). False: upstream's own controls
bool wheelkit_start();
bool wheelkit_on();
void wheelkit_window(void* hwnd);
// Window thread, once per frame: the profile's snapshot into the cabinet's
// controls while `driving`, else its menu buttons into wheelkit_menu_key().
// True when the wheel's Back asks for the pause menu
bool wheelkit_controls(Inputs& in, bool driving);
int wheelkit_menu_key();   // the next menu key (a VK_ code), 0 when none
// Game thread, every vblank: telemetry, and the drive board's motor value
// (port 0x2A) and mode (port 0x2E) logged as they change
void wheelkit_game_frame(int motor, int mode);
void wheelkit_stop();
#else
inline bool wheelkit_start() { return false; }
inline bool wheelkit_on() { return false; }
inline void wheelkit_window(void*) {}
inline bool wheelkit_controls(Inputs&, bool) { return false; }
inline int wheelkit_menu_key() { return 0; }
inline void wheelkit_game_frame(int, int) {}
inline void wheelkit_stop() {}
#endif
