// The Wheelkit runtime's loader and call sites (wheelkit_host.h). NOT part of
// upstream SegaRally2Native (snicolast): a modified version's addition.
#include "wheelkit_host.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>

#include "../video/vk_presenter.h"
#include "inputs.h"
#include "ppc_rt.h"
#include "wheelkit.h"
#include "wheelkit_model3.h"
#include "proxy/dbce_proxy_side_black.hpp"   // the runtime's black side screens (header only)
#include "frames/frame_burst.hpp"            // the runtime's frame bursts (header only)
#include <cstdarg>
#include <cstring>
#include <vector>

namespace {

#define X(ret, name, args) ret(WK_CALL* p_##name) args = nullptr;
WK_FUNCTIONS(X)
#undef X

constexpr const char* VERSION = "0.5.5-alpha";   // this build's, by Wheelkit's scheme (not upstream's)
// The drive board's full torque (0x40 from the centre) as a part of the wheel's own maximum, at Steering strength 50.
// Set against art of rally's held corner at turn-in (0.22 to 0.25): at 0.4 SEGA Rally 2's read 0.13 to 0.17 (2026-11-14)
constexpr float FORCE_SCALE = 0.6f;
bool g_on = false, g_panel = false, g_attached = false;
int g_keys[16], g_key_count = 0;   // window thread only
wk_triples g_screens{};
std::atomic<float> g_heading{0.0f};  // radians, clockwise from above: the camera's heading (window thread)
std::atomic<bool> g_racing{false};  // a race is on (game thread): three views only then, the centre screen otherwise
bool g_three = false;              // three projected views over the three screens (the rig's measurements)
std::atomic<unsigned long long> g_world_at{0};   // when the 3D world was last drawn (a course's title card has none)
std::atomic<unsigned long long> g_world_since{0};   // since when it has been drawn without a gap
wk_model3_servo g_servo;           // game thread only
unsigned g_frames = 0;

// Game RAM (docs/game_ram.md)
constexpr uint32_t SEQUENCE = 0x22D618, SELECT_STEP = 0x22435C, RACE_RESULT = 0x22D630, RACE_TIME = 0x22D080;
constexpr uint32_t SPEED = 0x22F3A8, HUD_GEAR = 0x13AB80, HUD_LAP = 0x13AB94, HUD_POSITION = 0x13AB74, TRANSMISSION = 0x224334;
constexpr uint32_t VIEW = 0x2244EC;   // VIEW CHANGE: 4 the first view, 5 the other (found by RAM dumps around presses; byte)

// The F6 panel's pages (the runtime owns the force and Steering strength rows). Screens and telemetry follow the
// rig profile Wheelkit writes into save\wheelkit.ini ([Triple] Screens, [Telemetry] Enabled): no row for them
constexpr char PAGES[] = R"({"settings":{"product":"SEGA Rally 2","version":"%s","keys":"F6 settings, F8 force feedback off","pages":{
"controls":[{"id":"controls.profile","kind":"status","label":"Wheel profile","text":"No wheel set up"}],
"ffb":[{"id":"ffb.status","kind":"status","text":"Off"},
{"id":"ffb.enabled","kind":"toggle","label":"Force feedback","default":true,"help":"The arcade cabinet's own force. F8 turns it off."},
{"id":"ffb.steering","kind":"percent","label":"Steering strength","default":50,"help":"50%% is the tuned strength; 100%% doubles it."}],
"display":[{"id":"display.status","kind":"status","label":"Now","text":"One screen"}],
"help":[{"id":"help.runtime","kind":"status","label":"Wheelkit runtime","text":"not loaded"}]}}})";

void WK_CALL log_line(const char* line, void*) { printf("[wheelkit] %s\n", line); }

// F6 > Controls and the log: whether the wheel is set up (Wheelkit writes [Controls] from the rig profile at Play)
void controls_status(const wk_input& s) {
    static int shown = -1;
    int now = !(s.flags & WK_INPUT_PROFILE) ? 0 : (s.flags & WK_INPUT_DEVICES) ? 2 : 1;
    if (now == shown) return;
    shown = now;
    std::string text = now == 0 ? "No wheel set up: Wheelkit sets it from your rig profile when you press Play on SEGA Rally 2 "
                                  "(the keyboard and gamepad work)"
                                : std::string(p_wk_input_profile()) + (now == 1 ? ": its wheel is not connected" : "");
    if (g_panel) p_wk_panel_set_text("controls.profile", text.c_str(), now == 2 ? WK_TONE_GOOD : WK_TONE_WARNING);
    p_wk_log(("sega-rally-2: wheel profile: " + text).c_str());
}

int adc(float v) { return std::clamp(int(std::lround(v)), 0, 255); }

}  // namespace

bool wheelkit_on() { return g_on; }
bool wheelkit_panel_open() { return g_panel && p_wk_panel_wants_input(); }

bool wheelkit_start() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    while (n && exe[n - 1] != L'\\') n--;
    std::wstring dir(exe, n);
    HMODULE m = LoadLibraryW((dir + L"wheelkit.dll").c_str());
    if (!m) {
        printf("[wheelkit] wheelkit.dll is not beside the program: the game's own controls\n");
        return false;
    }
    bool all = true;
#define X(ret, name, args)                                                           \
    p_##name = reinterpret_cast<ret(WK_CALL*) args>(GetProcAddress(m, #name)); \
    all = all && p_##name;
    WK_FUNCTIONS(X)
#undef X
    if (!all || p_wk_abi_version() != WK_ABI_VERSION) {
        printf("[wheelkit] wheelkit.dll is another version (ABI %d wanted): the game's own controls\n", WK_ABI_VERSION);
        return false;
    }
    char settings[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, (dir + L"save\\wheelkit.ini").c_str(), -1, settings, sizeof settings, nullptr, nullptr);
    CreateDirectoryW((dir + L"save").c_str(), nullptr);
    wk_identity id{};
    id.size = sizeof id;
    id.game_id = "sega-rally-2";
    id.game_name = "SEGA Rally 2";
    id.game_version = VERSION;
    id.settings_file = settings;
    id.log = log_line;
    if (!p_wk_start(&id)) {
        printf("[wheelkit] not started (%s): the game's own controls\n", p_wk_last_error());
        return false;
    }
    g_on = true;
    wk_model3_reset(&g_servo);
    p_wk_log("sega-rally-2: the runtime has the wheel; the game's own SDL wheel code is off");
    char pages[sizeof PAGES + 32];
    int length = snprintf(pages, sizeof pages, PAGES, VERSION);
    g_panel = p_wk_panel_configure(pages, size_t(length)) != 0;
    if (g_panel) p_wk_panel_set_text("help.runtime", p_wk_runtime_version(), WK_TONE_NORMAL);
    static const wk_camera_view VIEWS[] = {{"Bumper", "Bumper", 1}, {"Chase", "Chase", 0}};   // the cabinet's two views
    p_wk_cameras_configure(VIEWS, 2);
    return true;
}

// Before the window: the layout (the rig profile's [Triple] Screens, else the displays). With the rig's measurements
// the window covers the three screens and the 3D world is drawn three times at the rig's angles (wheelkit_views); the
// 2D screens and the HUD stay in the middle
void wheelkit_display(float& aspect, bool& hud_centre) {
    if (!g_on) return;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // the layout is in real pixels
    g_screens.size = sizeof g_screens;
    if (!p_wk_triples_get(&g_screens)) g_screens.span = 0;
    else if (g_panel) p_wk_panel_set_text("display.status", g_screens.text, WK_TONE_NORMAL);
    g_three = g_screens.span && g_screens.rig && g_screens.height > 0;
    if (g_three) aspect = float(g_screens.width) / float(g_screens.height), hud_centre = true;
}

// On three screens without the rig's measurements the game stays on the centre one, at its own shape, and the side
// screens are black (one flat picture over three is a stretched picture)
void wheelkit_window(void* hwnd) {
    if (!g_on) return;
    p_wk_set_window(hwnd);
    if (!g_screens.span) return;
    HWND w = static_cast<HWND>(hwnd);
    const int x = g_screens.x, y = g_screens.y, cx = x + g_screens.center_x, cw = g_screens.center_width, h = g_screens.height;
    SetWindowLongA(w, GWL_STYLE, (GetWindowLongA(w, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    if (g_three) {
        SetWindowPos(w, HWND_TOP, x, y, g_screens.width, h, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        p_wk_log("sega-rally-2: three screens: three views at the rig's angles, the 2D screens and the HUD on the centre");
        return;
    }
    SetWindowPos(w, HWND_TOP, cx, y, cw, h, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    dbce::proxy::sideblack::Options o;
    o.sides[0] = RECT{x, y, cx, y + h};
    o.sides[1] = RECT{cx + cw, y, x + g_screens.width, y + h};
    o.log = [](const char* line) { p_wk_log(line); };
    dbce::proxy::sideblack::Start(o);
    p_wk_log("sega-rally-2: three screens: the game on the centre screen, the side screens black");
}

// The three screens' projections for a full-screen 3D viewport of the game, left to right, from its projection `p`
// (column-major, Vulkan clip: y down, depth near / -z) and the widening that makes it fill one screen. The runtime's
// views take a centred Direct3D right-handed projection, row-vector and row-major: the same 16 numbers as a
// column-major one acting on columns, so rotation x projection there is this matrix here
int wheelkit_views(const float* p, float widen, float out[3][16], float ao[3][7]) {
    const unsigned long long now = GetTickCount64();
    if (now - g_world_at.load() > 250) g_world_since = now;
    g_world_at = now;
    if (!g_three) return 0;
    const double n = 0.1, f = 10000.0;
    double c[16] = {};
    c[0] = p[0] / widen, c[5] = std::fabs(p[5]), c[10] = f / (n - f), c[11] = -1, c[14] = n * f / (n - f);
    wk_triple_views tv{};
    tv.size = sizeof tv;
    if (!p_wk_triples_views(c, &tv)) return 0;
    static int logged = 0;
    if (g_racing && logged++ == 0) {   // once: the game's projection and the side screens' (the seams' checks read it)
        char line[300];
        snprintf(line, sizeof line, "sega-rally-2: three views from the game's projection %.4f %.4f %.4f %.4f widen %.3f; left %.4f %.4f %.4f, right %.4f %.4f %.4f",
                 p[0], p[5], p[8], p[9], widen, tv.projection[0][0], tv.projection[0][8], tv.rotation[0][2], tv.projection[2][0], tv.projection[2][8], tv.rotation[2][2]);
        p_wk_log(line);
    }
    const int count = g_racing ? 3 : 1;   // 1: the centre screen alone (views[1]), the sides black
    for (int s = 0; s < 3; s++) {
        double m[16];
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) {
                m[r * 4 + k] = 0;
                for (int j = 0; j < 4; j++) m[r * 4 + k] += tv.rotation[s][r * 4 + j] * tv.projection[s][j * 4 + k];
            }
        for (int r = 0; r < 4; r++) {   // row k of the clip position is m[r * 4 + k] over r
            out[s][r * 4 + 0] = float(m[r * 4 + 0] - p[8] / widen * m[r * 4 + 3]);   // the game's own shear, as at the centre
            out[s][r * 4 + 1] = float(-m[r * 4 + 1] - p[9] * m[r * 4 + 3]);   // y down
            out[s][r * 4 + 2] = r == 3 ? p[14] : 0.0f;                        // depth: near / -z, as the game's
            out[s][r * 4 + 3] = float(m[r * 4 + 3]);
        }
        if (!ao) continue;   // the screen's own projection (x and y scale and offset) and the yaw back to the centre's view
        const double* q = tv.projection[s];
        const double* rot = tv.rotation[s];
        float a[7] = {float(q[0]), float(-q[5]), float(q[8] + p[8] / widen), float(-q[9] + p[9]), float(rot[0]), float(rot[2]), float(rot[8])};
        if (s == 1) a[4] = a[5] = a[6] = 0;   // the centre: no turn
        for (int i = 0; i < 7; i++) ao[s][i] = a[i];
    }
    return count;
}
bool wheelkit_three_screens() { return g_three; }

// Frame bursts: %LOCALAPPDATA%\Wheelkit\sega-rally-2\frames.txt asks for one (a fenced test run or the developer switch)
static wheelkit::frames::Burst g_burst;
static unsigned g_burst_index = 0;
static void burst_log(const char* fmt, ...) {
    char line[600];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    p_wk_log(line);
}
bool wheelkit_frames_poll() {
    if (!g_on) return false;
    if (!g_burst.Active()) {
        char folder[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", folder, MAX_PATH - 40);
        if (!n || n >= MAX_PATH - 40) return false;
        strcat_s(folder, "\\Wheelkit\\sega-rally-2\\");
        wk_ffb_status st{};
        st.size = sizeof st;
        g_burst.Poll(folder, p_wk_ffb_get_status(&st) && st.state == WK_FFB_FENCED, burst_log);
        g_burst_index = 0;
    }
    return g_burst.Active();
}
void wheelkit_frames_add(const unsigned char* bgra, unsigned width, unsigned height, size_t pitch) {
    static std::vector<unsigned char> small;
    const unsigned s = unsigned(g_burst.scale), w = width / s, h = height / s;
    if (s > 1) {   // every s-th pixel of every s-th row
        small.resize(size_t(w) * h * 4);
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++) memcpy(&small[(size_t(y) * w + x) * 4], bgra + size_t(y) * s * pitch + size_t(x) * s * 4, 4);
        bgra = small.data(), pitch = size_t(w) * 4;
    }
    g_burst.Add(g_burst_index++, bgra, w, h, pitch, g_frames, g_burst.ElapsedMs(), g_racing ? 3 : 1);
    if (int(g_burst_index) >= g_burst.wanted) g_burst.Finish(burst_log);
}

// The camera's heading over the ground: its forward axis (view -z) in the world's x and z. The game's own heading is not
// in its known RAM; the camera rides with the car, so it turns with it
void wheelkit_camera(const float* a) {
    if (g_on && (a[2] != 0 || a[8] != 0)) g_heading = std::atan2(-a[2], -a[8]);
}

void wheelkit_frame(const VkPresenter& vk, void* cmd, void* view, unsigned width, unsigned height) {
    if (!g_panel) return;
    if (!g_attached) {
        wk_panel_vk_host host{};
        host.size = sizeof host;
        host.window = vk.hwnd();
        host.instance = vk.instance();
        host.physical_device = vk.gpu();
        host.device = vk.device();
        host.queue = vk.queue();
        host.queue_family = vk.queue_family();
        host.image_count = 3;   // one command buffer, and frame() waits for the frame before: two would do
        host.color_format = unsigned(vk.swap_format());
        g_attached = p_wk_panel_vk_attach(&host) != 0;
        if (!g_attached) { g_panel = false; return; }
    }
    p_wk_panel_vk_frame(cmd, reinterpret_cast<unsigned long long>(view), int(width), int(height));
}

bool wheelkit_controls(Inputs& in, bool driving) {
    if (!g_on) return false;
    wk_panel_event e{};
    e.size = sizeof e;
    while (g_panel && p_wk_panel_poll_event(&e)) {}   // every row is the runtime's own: polling lets it handle them
    static int tick = 0;
    if (g_panel && tick++ % 30 == 0) {
        wk_ffb_status st{};
        st.size = sizeof st;
        if (p_wk_ffb_get_status(&st)) p_wk_panel_set_text("ffb.status", st.text, st.state == WK_FFB_ON ? WK_TONE_GOOD : WK_TONE_NORMAL);
    }
    wk_input s{};
    s.size = sizeof s;
    if (!p_wk_input_poll(&s)) return false;
    controls_status(s);
    if (!(s.flags & (WK_INPUT_PROFILE | WK_INPUT_INJECTED)) || (s.flags & WK_INPUT_PANEL)) return false;
    auto pressed = [&](int action) { return s.pressed[action] != 0; };
    if (!driving) {   // a menu of the port has the controls (or the window is in the background)
        static const int MENU[][2] = {{WK_ACTION_NAV_UP, VK_UP},        {WK_ACTION_NAV_DOWN, VK_DOWN},
                                      {WK_ACTION_NAV_LEFT, VK_LEFT},    {WK_ACTION_NAV_RIGHT, VK_RIGHT},
                                      {WK_ACTION_CONFIRM, VK_RETURN},   {WK_ACTION_START, VK_RETURN},
                                      {WK_ACTION_BACK, VK_ESCAPE}};
        for (const auto& k : MENU)
            if (pressed(k[0]) && g_key_count < 16) g_keys[g_key_count++] = k[1];
        return false;
    }
    // Added to the keyboard's, as the pads are (app_input.cpp): the stronger steering of the two
    if (std::fabs(s.steer) >= std::fabs((float(in.steer) - 128.0f) / 127.0f)) in.steer = adc(128.0f + 127.0f * s.steer);
    in.accel = std::max(int(in.accel), adc(255.0f * s.throttle));
    in.brake = std::max(int(in.brake), adc(255.0f * s.brake));
    if (s.handbrake > 0.5f || s.held[WK_ACTION_HANDBRAKE]) in.handbrake = true;
    if (s.held[WK_ACTION_CAMERA]) in.view = true;   // VIEW CHANGE
    if ((s.bound & WK_BOUND_GEARS) && s.transmission == WK_TRANSMISSION_H_PATTERN) {
        in.gear = std::clamp(s.gear, 0, 4);   // the cabinet's 4-speed H shifter; 0 = neutral
    } else {
        if (pressed(WK_ACTION_SHIFT_UP)) in.gear = std::min(std::max(int(in.gear), 1) + 1, 4);
        if (pressed(WK_ACTION_SHIFT_DOWN)) in.gear = std::max(int(in.gear) - 1, 1);
    }
    if (pressed(WK_ACTION_SELECT)) press_coin();
    if (pressed(WK_ACTION_START)) press_start();
    static bool moved = false, coined = false;
    if (!moved && (s.steer != 0 || s.throttle > 0 || s.brake > 0)) {
        moved = true;
        p_wk_log("sega-rally-2: the profile's input reached the cabinet's controls (wheel or pedals)");
    }
    if (!coined && (pressed(WK_ACTION_SELECT) || pressed(WK_ACTION_START))) {
        coined = true;
        p_wk_log("sega-rally-2: the profile's input reached the cabinet's controls (coin or start)");
    }
    return pressed(WK_ACTION_BACK);
}

int wheelkit_menu_key() { return g_key_count > 0 ? g_keys[--g_key_count] : 0; }

// Game thread, from the drive board's Z80 program: a value written to the servo register `reg` (wheelkit_model3.h)
void wheelkit_motor(int reg, int value) {
    if (!g_on) return;
    static FILE* trace = getenv("SR2_DRIVE_TRACE") ? fopen(getenv("SR2_DRIVE_TRACE"), "w") : nullptr;   // a test recording
    static int last = -1;
    if (trace && (reg << 8 | value) != last) fprintf(trace, "%u %02X %02X %02X\n", g_frames, reg, value, int(g_inputs.steer) & 0xFF);
    last = reg << 8 | value;
    bool idle = g_servo.idle != 0;
    wk_model3_write(&g_servo, WK_MODEL3_PORT_REGISTER, reg);
    wk_model3_write(&g_servo, WK_MODEL3_PORT_VALUE, value);
    static int logged = 0;
    if (idle != (g_servo.idle != 0) && logged++ < 200)
        p_wk_log(g_servo.idle ? "sega-rally-2: drive board idle (register FF): no force" : "sega-rally-2: drive board motor on (torque register FE)");
}

void wheelkit_game_frame() {
    g_frames++;
    if (!g_on) return;
    // The board's torque and spring as one force, a little smoothed (the game's torque steps every frame)
    static float force = 0;
    float steer = std::clamp((float(g_inputs.steer) - 128.0f) / 127.0f, -1.0f, 1.0f);
    force += 0.5f * (wk_model3_force(&g_servo, steer) * FORCE_SCALE - force);
    if (g_servo.idle) force = 0;
    p_wk_ffb_set_force(force);
    if (wk_model3_vibration(&g_servo) > 0) p_wk_ffb_cue(WK_CUE_RUMBLE, wk_model3_vibration(&g_servo), 100);
    char line[200];
    static float low = 0, high = 0;
    static int frames = 0;
    low = std::min(low, force), high = std::max(high, force);
    if (!g_servo.idle && ++frames % 600 == 0 && frames <= 12000) {
        snprintf(line, sizeof line, "sega-rally-2: force asked in the last 600 frames %.3f to %.3f (drive board torque %02X, spring %d, register FD %02X)",
                 low, high, g_servo.torque, g_servo.spring, g_servo.undecoded);
        p_wk_log(line);
        low = high = 0;
    }
    wk_car car{};
    car.size = sizeof car;
    // The race once the world has been on screen for 1.5 s: not during the course's title card (2D, about 5 s), nor the
    // second of the course before it (the drive board's first command there reached 0.041 at 0.6)
    const unsigned long long now = GetTickCount64();
    car.race_on = int(rd32(SEQUENCE)) == 1 && rd32(SELECT_STEP) == 17 && rd32(RACE_RESULT) <= 1 && now - g_world_at.load() < 250 &&
                  now - g_world_since.load() >= 1500;
    float speed = std::bit_cast<float>(rd32(SPEED));
    car.speed = std::isfinite(speed) ? speed : 0.0f;
    // Manual: the cabinet's shifter as the game reads it (0 neutral, as its HUD's N); HUD_GEAR keeps 1 in neutral
    car.gear = std::clamp(rd32(TRANSMISSION) ? int(g_inputs.gear) : int(rd32(HUD_GEAR)), 0, 4);
    car.throttle = float(g_inputs.accel) / 255.0f;
    car.brake = float(g_inputs.brake) / 255.0f;
    car.handbrake = g_inputs.handbrake ? 1.0f : 0.0f;
    car.steer = steer;
    car.yaw = car.race_on ? g_heading.load() : 0.0f;
    g_racing = car.race_on != 0;
    if (car.race_on) {   // the view in use, for the runtime's cameras (its name reaches the proof lines)
        wk_camera cam{};
        cam.size = sizeof cam;
        p_wk_cameras_frame(rd8(VIEW) == 5 ? 1 : 0, nullptr, 0, &cam);
    }
    car.race_time = float(rd32(RACE_TIME)) / 6000.0f;
    car.lap = int(rd32(HUD_LAP)) + 1;
    car.race_position = int(rd32(HUD_POSITION)) + 1;
    p_wk_telemetry_frame(&car);   // the engine speed is not found in the game's RAM yet: sent as 0
    // The log: the select screens' steps, and once a second for two minutes of racing what was sent and the cabinet's
    // controls the game read (a test run reads the wheel's effect back from these lines)
    static int step = -1, steps = 0, race_frames = 0;
    if (int(rd32(SELECT_STEP)) != step && steps++ < 60) {
        step = int(rd32(SELECT_STEP));
        snprintf(line, sizeof line, "sega-rally-2: select step %d, transmission %s", step, rd32(TRANSMISSION) ? "MT" : "AT");
        p_wk_log(line);
    }
    if (car.race_on && race_frames++ % 60 == 0 && race_frames < 60 * 120) {
        snprintf(line, sizeof line, "sega-rally-2: telemetry in a race: %.1f m/s, gear %d, lap %d, position %d, %.2f s; "
                 "cabinet steer %d accel %d brake %d lever %d; force asked %.3f; camera heading %.0f",
                 car.speed, car.gear, car.lap, car.race_position, car.race_time, int(g_inputs.steer), int(g_inputs.accel),
                 int(g_inputs.brake), int(g_inputs.gear), force, car.yaw * 57.29578f);
        p_wk_log(line);
    }
}

void wheelkit_stop() {
    if (g_on) p_wk_stop();   // detaches the panel: before the presenter's device goes
    g_on = g_panel = false;
}
