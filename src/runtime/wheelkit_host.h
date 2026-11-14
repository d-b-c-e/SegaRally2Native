// The Wheelkit runtime (wheelkit.dll, loaded beside the program when it is
// there): the wheel profile's input into the cabinet's controls, telemetry
// from the game's RAM, the drive board's motor output as force feedback, the
// F6 panel on the frame, and the window over three screens.
// NOT part of upstream SegaRally2Native (snicolast): a modified version's
// addition. Built only with -DWHEELKIT_SDK=<wheelkit checkout>; without it, or
// without the DLL, the game is upstream's.
#pragma once

struct Inputs;
class VkPresenter;

#ifdef SR2_WHEELKIT
// Loads and starts the runtime (no window yet). False: upstream's own controls
bool wheelkit_start();
bool wheelkit_on();
// Before the window: the layout of the screens; on three, the game stays on the centre one
void wheelkit_display(float& aspect, bool& hud_centre);
// The three screens' projections for a full-screen 3D viewport (projection p, widened by widen to fill one
// screen), left to right; 0 when the game draws one view
int wheelkit_views(const float* p, float widen, float out[3][16], float ao[3][7] = nullptr);
bool wheelkit_three_screens();   // the window covers the three screens (2D-only pictures go on the centre one)
// Frame bursts (the runtime's runtime/native/frames): each image, true while one runs; its images measured in _add
bool wheelkit_frames_poll();
void wheelkit_frames_add(const unsigned char* bgra, unsigned width, unsigned height, size_t pitch);
// Window thread, each new frame of the world: its x, y (up) and z axes in the camera's view space
void wheelkit_camera(const float* world_axes);
void wheelkit_window(void* hwnd);
// Window thread, once per frame: the profile's snapshot into the cabinet's
// controls while `driving`, else its menu buttons into wheelkit_menu_key().
// True when the wheel's Back asks for the pause menu
bool wheelkit_controls(Inputs& in, bool driving);
int wheelkit_menu_key();     // the next menu key (a VK_ code), 0 when none
bool wheelkit_panel_open();  // the F6 panel has the keyboard
// In the presenter's frame, after the game's own layers: the F6 panel onto
// the swapchain image (VkCommandBuffer, VkImageView)
void wheelkit_frame(const VkPresenter& vk, void* cmd, void* view, unsigned width, unsigned height);
// Game thread: a value the drive board's program wrote to the servo register
// `reg` (ports 0x2E and 0x2A), and every vblank: the force and telemetry
void wheelkit_motor(int reg, int value);
void wheelkit_game_frame();
void wheelkit_stop();
#else
inline bool wheelkit_start() { return false; }
inline bool wheelkit_on() { return false; }
inline void wheelkit_display(float&, bool&) {}
inline int wheelkit_views(const float*, float, float (*)[16], float (*)[7] = nullptr) { return 0; }
inline bool wheelkit_three_screens() { return false; }
inline bool wheelkit_frames_poll() { return false; }
inline void wheelkit_frames_add(const unsigned char*, unsigned, unsigned, size_t) {}
inline void wheelkit_camera(const float*) {}
inline void wheelkit_window(void*) {}
inline bool wheelkit_controls(Inputs&, bool) { return false; }
inline int wheelkit_menu_key() { return 0; }
inline bool wheelkit_panel_open() { return false; }
inline void wheelkit_frame(const VkPresenter&, void*, void*, unsigned, unsigned) {}
inline void wheelkit_motor(int, int) {}
inline void wheelkit_game_frame() {}
inline void wheelkit_stop() {}
#endif
