// The Wheelkit runtime's loader and call sites (wheelkit_host.h). NOT part of
// upstream SegaRally2Native (snicolast): a modified version's addition.
#include "wheelkit_host.h"

#include <algorithm>
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

namespace {

#define X(ret, name, args) ret(WK_CALL* p_##name) args = nullptr;
WK_FUNCTIONS(X)
#undef X

constexpr const char* VERSION = "0.4.2-alpha";   // this build's, by Wheelkit's scheme (not upstream's)
// The drive board's full torque (0x40 from the centre) as a part of the wheel's own maximum, at Steering strength 50.
// Its sign is checked in an injected race (the force asked pulls toward the centre); the feel is the owner's to judge
constexpr float FORCE_SCALE = 0.4f;
bool g_on = false, g_panel = false, g_attached = false;
int g_keys[16], g_key_count = 0;   // window thread only
wk_triples g_screens{};
wk_model3_servo g_servo;           // game thread only
unsigned g_frames = 0;

// Game RAM (docs/game_ram.md)
constexpr uint32_t SEQUENCE = 0x22D618, SELECT_STEP = 0x22435C, RACE_RESULT = 0x22D630, RACE_TIME = 0x22D080;
constexpr uint32_t SPEED = 0x22F3A8, HUD_GEAR = 0x13AB80, HUD_LAP = 0x13AB94, HUD_POSITION = 0x13AB74, TRANSMISSION = 0x224334;

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
    return true;
}

// Before the window: the layout (the rig profile's [Triple] Screens, else the displays)
void wheelkit_display() {
    if (!g_on) return;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // the layout is in real pixels
    g_screens.size = sizeof g_screens;
    if (!p_wk_triples_get(&g_screens)) g_screens.span = 0;
    else if (g_panel) p_wk_panel_set_text("display.status", g_screens.text, WK_TONE_NORMAL);
}

// On three screens the game stays on the centre one, at its own shape, and the side screens are black (one flat
// picture over three is a stretched picture; three angled views are not built yet)
void wheelkit_window(void* hwnd) {
    if (!g_on) return;
    p_wk_set_window(hwnd);
    if (!g_screens.span) return;
    HWND w = static_cast<HWND>(hwnd);
    const int x = g_screens.x, y = g_screens.y, cx = x + g_screens.center_x, cw = g_screens.center_width, h = g_screens.height;
    SetWindowLongA(w, GWL_STYLE, (GetWindowLongA(w, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    SetWindowPos(w, HWND_TOP, cx, y, cw, h, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    dbce::proxy::sideblack::Options o;
    o.sides[0] = RECT{x, y, cx, y + h};
    o.sides[1] = RECT{cx + cw, y, x + g_screens.width, y + h};
    o.log = [](const char* line) { p_wk_log(line); };
    dbce::proxy::sideblack::Start(o);
    p_wk_log("sega-rally-2: three screens: the game on the centre screen, the side screens black");
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
    car.race_on = int(rd32(SEQUENCE)) == 1 && rd32(SELECT_STEP) == 17 && rd32(RACE_RESULT) <= 1;
    float speed = std::bit_cast<float>(rd32(SPEED));
    car.speed = std::isfinite(speed) ? speed : 0.0f;
    car.gear = std::clamp(int(rd32(HUD_GEAR)), 0, 4);
    car.throttle = float(g_inputs.accel) / 255.0f;
    car.brake = float(g_inputs.brake) / 255.0f;
    car.handbrake = g_inputs.handbrake ? 1.0f : 0.0f;
    car.steer = steer;
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
                 "cabinet steer %d accel %d brake %d lever %d; force asked %.3f",
                 car.speed, car.gear, car.lap, car.race_position, car.race_time, int(g_inputs.steer), int(g_inputs.accel),
                 int(g_inputs.brake), int(g_inputs.gear), force);
        p_wk_log(line);
    }
}

void wheelkit_stop() {
    if (g_on) p_wk_stop();   // detaches the panel: before the presenter's device goes
    g_on = g_panel = false;
}
