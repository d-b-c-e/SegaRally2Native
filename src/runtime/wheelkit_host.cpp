// The Wheelkit runtime's loader and call sites (wheelkit_host.h). NOT part of
// upstream SegaRally2Native (snicolast): a modified version's addition.
#include "wheelkit_host.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>

#include "inputs.h"
#include "ppc_rt.h"
#include "wheelkit.h"

namespace {

#define X(ret, name, args) ret(WK_CALL* p_##name) args = nullptr;
WK_FUNCTIONS(X)
#undef X

constexpr const char* VERSION = "0.2.0-alpha";   // this build's, by Wheelkit's scheme (not upstream's)
bool g_on = false;
int g_keys[16], g_key_count = 0;   // window thread only

// Game RAM (docs/game_ram.md)
constexpr uint32_t SEQUENCE = 0x22D618, SELECT_STEP = 0x22435C, RACE_RESULT = 0x22D630, RACE_TIME = 0x22D080;
constexpr uint32_t SPEED = 0x22F3A8, HUD_GEAR = 0x13AB80, HUD_LAP = 0x13AB94, HUD_POSITION = 0x13AB74;

void WK_CALL log_line(const char* line, void*) { printf("[wheelkit] %s\n", line); }

int adc(float v) { return std::clamp(int(std::lround(v)), 0, 255); }

}  // namespace

bool wheelkit_on() { return g_on; }

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
    p_wk_log("sega-rally-2: the runtime has the wheel; the game's own SDL wheel code is off, and no force is sent yet");
    return true;
}

void wheelkit_window(void* hwnd) {
    if (g_on) p_wk_set_window(hwnd);
}

bool wheelkit_controls(Inputs& in, bool driving) {
    if (!g_on) return false;
    wk_input s{};
    s.size = sizeof s;
    if (!p_wk_input_poll(&s) || !(s.flags & (WK_INPUT_PROFILE | WK_INPUT_INJECTED))) return false;
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
    // The axes only while every device of the profile is connected: a missing one does not read as neutral
    bool axes = (s.flags & WK_INPUT_DEVICES) != 0;
    if (axes) {
        if (std::fabs(s.steer) >= std::fabs((float(in.steer) - 128.0f) / 127.0f)) in.steer = adc(128.0f + 127.0f * s.steer);
        in.accel = std::max(int(in.accel), adc(255.0f * s.throttle));
        in.brake = std::max(int(in.brake), adc(255.0f * s.brake));
    }
    if ((axes && s.handbrake > 0.5f) || s.held[WK_ACTION_HANDBRAKE]) in.handbrake = true;
    if (s.held[WK_ACTION_CAMERA]) in.view = true;   // VIEW CHANGE
    if ((s.bound & WK_BOUND_GEARS) && s.transmission == WK_TRANSMISSION_H_PATTERN) {
        in.gear = std::clamp(s.gear, 0, 4);   // the cabinet's 4-speed H shifter; 0 = neutral
    } else {
        if (pressed(WK_ACTION_SHIFT_UP)) in.gear = std::min(std::max(int(in.gear), 1) + 1, 4);
        if (pressed(WK_ACTION_SHIFT_DOWN)) in.gear = std::max(int(in.gear) - 1, 1);
    }
    if (pressed(WK_ACTION_SELECT)) press_coin();
    if (pressed(WK_ACTION_START)) press_start();
    static bool reached = false;
    bool moved = axes && (s.steer != 0 || s.throttle > 0 || s.brake > 0);
    if (!reached && (moved || pressed(WK_ACTION_SELECT) || pressed(WK_ACTION_START))) {
        reached = true;
        p_wk_log(moved ? "sega-rally-2: the profile's input reached the cabinet's controls (wheel or pedals)"
                       : "sega-rally-2: the profile's input reached the cabinet's controls (coin or start)");
    }
    return pressed(WK_ACTION_BACK);
}

int wheelkit_menu_key() { return g_key_count > 0 ? g_keys[--g_key_count] : 0; }

void wheelkit_game_frame(int motor, int mode) {
    if (!g_on) return;
    char line[160];
    static int last_motor = -1, last_mode = -1, logged = 0;
    if (mode != last_mode || (motor != last_motor && logged < 60)) {   // for the force model still to be written
        snprintf(line, sizeof line, "sega-rally-2: drive board motor %02X mode %02X, wheel %02X", motor, mode,
                 int(g_inputs.steer) & 0xFF);
        p_wk_log(line);
        last_motor = motor, last_mode = mode, logged++;
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
    car.steer = std::clamp((float(g_inputs.steer) - 128.0f) / 127.0f, -1.0f, 1.0f);
    car.race_time = float(rd32(RACE_TIME)) / 6000.0f;
    car.lap = int(rd32(HUD_LAP)) + 1;
    car.race_position = int(rd32(HUD_POSITION)) + 1;
    p_wk_telemetry_frame(&car);   // the engine speed is not found in the game's RAM yet: sent as 0
    static int race_frames = 0;
    if (car.race_on && race_frames++ % 600 == 0 && race_frames < 6000) {
        snprintf(line, sizeof line, "sega-rally-2: telemetry in a race: %.1f m/s, gear %d, lap %d, position %d, %.2f s",
                 car.speed, car.gear, car.lap, car.race_position, car.race_time);
        p_wk_log(line);
    }
}

void wheelkit_stop() {
    if (g_on) p_wk_stop();
    g_on = false;
}
