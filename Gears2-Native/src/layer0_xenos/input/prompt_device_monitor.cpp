// Layer 0 (game-free): see xenos/input/prompt_device_monitor.h.
// Contributed by Flippage (COGBox PR #117); portions from gow3-recomp: BSD-3-Clause, Copyright (c) Flippage and the
// gow3-recomp contributors (THIRD_PARTY_NOTICES.md). The rest is our code (MIT).
#include <xenos/input/prompt_device_monitor.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>
#endif

// Rework of PR #117: "auto" by default, but an Xbox / XInput pad keeps the game's own prompts (no overlay); the other
// pads get their Kenney glyphs and keyboard / mouse the key caps. "original" = never touch the prompts (as before).
REXCVAR_DEFINE_STRING(gow_button_prompts, "auto", "Input",
                      "Button prompts: auto (Xbox pad = the game's own; PlayStation / Switch / Steam pads and keyboard "
                      "get their own, switching live), xbox, playstation, switch, steamdeck, steamcontroller, keyboard, "
                      "original (never change the game's prompts)");
REXCVAR_DEFINE_STRING(gow2_button_prompts, "auto", "Input", "Alias of --gow_button_prompts (used when that one is auto)");
// GitHub #104: the launcher's "Controller button prompts" option (keep the pad prompts with Keyboard and mouse on).
REXCVAR_DEFINE_BOOL(gow_button_prompts_keep_pad, false, "Input",
                    "Auto button prompts never switch to key caps (keyboard / mouse use keeps the pad's prompts)");

namespace xenos::input {

namespace {

std::string ToLowerTrim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  std::string out(s.size(), '\0');
  for (size_t i = 0; i < s.size(); ++i) {
    out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
  }
  return out;
}

bool ContainsCI(std::string_view haystack, std::string_view needle_lower) {
  if (haystack.empty() || needle_lower.empty() || needle_lower.size() > haystack.size()) return false;
  return std::search(haystack.begin(), haystack.end(), needle_lower.begin(), needle_lower.end(),
                     [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
                     }) != haystack.end();
}

const char* const kKeybindCvars[] = {
    "keybind_right_trigger",
    "keybind_left_trigger",
    "keybind_a",
    "keybind_b",
    "keybind_x",
    "keybind_y",
    "keybind_right_shoulder",
    "keybind_left_shoulder",
    "keybind_dpad_left",
    "keybind_dpad_right",
    "keybind_dpad_up",
    "keybind_dpad_down",
    "keybind_rstick_press",
    "keybind_lstick_press",
    "keybind_start",
    "keybind_back",
};

#ifdef _WIN32
// SDL3's gamepad list, for classification only: the *ForID queries need no open gamepad, so none is ever opened (the
// PR opened one per poll and never closed it).
struct SdlGamepadApi {
  using SDL_JoystickID = uint32_t;
  using GetGamepadsFn = SDL_JoystickID*(__cdecl*)(int* count);
  using FreeFn = void(__cdecl*)(void* mem);
  using GetGamepadTypeForIDFn = int(__cdecl*)(SDL_JoystickID instance_id);
  using GetGamepadVendorForIDFn = uint16_t(__cdecl*)(SDL_JoystickID instance_id);
  using GetGamepadProductForIDFn = uint16_t(__cdecl*)(SDL_JoystickID instance_id);
  using GetGamepadNameForIDFn = const char*(__cdecl*)(SDL_JoystickID instance_id);

  HMODULE mod = nullptr;
  GetGamepadsFn GetGamepads = nullptr;
  FreeFn Free = nullptr;
  GetGamepadTypeForIDFn GetGamepadTypeForID = nullptr;
  GetGamepadVendorForIDFn GetGamepadVendorForID = nullptr;
  GetGamepadProductForIDFn GetGamepadProductForID = nullptr;
  GetGamepadNameForIDFn GetGamepadNameForID = nullptr;

  bool Resolve() {
    HMODULE current = GetModuleHandleW(L"SDL3.dll");  // only the SDL the game already loaded
    if (!current) return false;
    if (mod == current && GetGamepads) return true;
    mod = current;
    GetGamepads = reinterpret_cast<GetGamepadsFn>(GetProcAddress(mod, "SDL_GetGamepads"));
    Free = reinterpret_cast<FreeFn>(GetProcAddress(mod, "SDL_free"));
    GetGamepadTypeForID = reinterpret_cast<GetGamepadTypeForIDFn>(GetProcAddress(mod, "SDL_GetGamepadTypeForID"));
    GetGamepadVendorForID = reinterpret_cast<GetGamepadVendorForIDFn>(GetProcAddress(mod, "SDL_GetGamepadVendorForID"));
    GetGamepadProductForID =
        reinterpret_cast<GetGamepadProductForIDFn>(GetProcAddress(mod, "SDL_GetGamepadProductForID"));
    GetGamepadNameForID = reinterpret_cast<GetGamepadNameForIDFn>(GetProcAddress(mod, "SDL_GetGamepadNameForID"));
    return GetGamepads != nullptr && Free != nullptr;
  }
};

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn ResolveXInputGetState() {
  static const XInputGetStateFn fn = [] {
    HMODULE xi = GetModuleHandleW(L"xinput1_4.dll");
    if (!xi) xi = GetModuleHandleW(L"xinput9_1_0.dll");
    if (!xi) xi = LoadLibraryW(L"xinput1_4.dll");
    if (!xi) xi = LoadLibraryW(L"xinput9_1_0.dll");
    return xi ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(xi, "XInputGetState")) : nullptr;
  }();
  return fn;
}

// Is one of our windows in front? A key held or a click made in another app is not the game's (and SDK patch 53 keeps
// the pad working while the game is in the background - its prompts must not flip to key caps then).
bool GameWindowInFront() {
  HWND fg = GetForegroundWindow();
  if (!fg) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}
#endif

}  // namespace

class PromptDeviceMonitor::WindowWatcher final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  explicit WindowWatcher(PromptDeviceMonitor& monitor) : monitor_(monitor) {}

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    monitor_.NotifyKeyDown(static_cast<uint32_t>(e.virtual_key()), PromptDeviceMonitor::NowMs());
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    monitor_.NotifyKeyUp(static_cast<uint32_t>(e.virtual_key()), PromptDeviceMonitor::NowMs());
  }

  void OnMouseDown(rex::ui::MouseEvent&) override {
    monitor_.NotifyMouseDown(PromptDeviceMonitor::NowMs());
  }
  void OnMouseUp(rex::ui::MouseEvent&) override {
    monitor_.NotifyMouseUp(PromptDeviceMonitor::NowMs());
  }

  void OnLostFocus(rex::ui::UISetupEvent&) override {
    monitor_.ResetInputStates();
  }

  void OnMouseWheel(rex::ui::MouseEvent&) override {
    monitor_.NotifyKeyboardMouseActivity(PromptDeviceMonitor::NowMs());
  }

  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (!has_last_pos_) {
      last_x_ = e.x();
      last_y_ = e.y();
      has_last_pos_ = true;
      return;
    }
    int32_t dx = 0;
    int32_t dy = 0;
    const bool captured = e.target() && e.target()->IsMouseCaptureRequested();
    if (e.dx() != 0.0f || e.dy() != 0.0f) {
      dx = static_cast<int32_t>(std::lround(e.dx()));
      dy = static_cast<int32_t>(std::lround(e.dy()));
    } else if (!captured) {
      const int32_t raw_dx = e.x() - last_x_;
      const int32_t raw_dy = e.y() - last_y_;
      if (std::abs(raw_dx) < 300 && std::abs(raw_dy) < 300) {
        dx = raw_dx;
        dy = raw_dy;
      }
    }
    last_x_ = e.x();
    last_y_ = e.y();
    if (dx != 0 || dy != 0) {
      monitor_.NotifyMouseMove(dx, dy, PromptDeviceMonitor::NowMs());
    }
  }

  void OnClosing(rex::ui::UIEvent& e) override { monitor_.DetachWindow(e.target()); }

 private:
  PromptDeviceMonitor& monitor_;
  int32_t last_x_ = 0;
  int32_t last_y_ = 0;
  bool has_last_pos_ = false;
};

PromptDeviceMonitor::PromptDeviceMonitor() = default;

PromptDeviceMonitor::~PromptDeviceMonitor() {
  Stop();
  DetachWindow(nullptr);
}

uint64_t PromptDeviceMonitor::NowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool PromptDeviceMonitor::IsSteamDeckEnvironment() {
#ifdef _WIN32
  char buf[16] = {};
  if (GetEnvironmentVariableA("SteamDeck", buf, sizeof(buf)) == 1 && buf[0] == '1') return true;
  if (GetEnvironmentVariableA("STEAM_DECK", buf, sizeof(buf)) == 1 && buf[0] == '1') return true;
  return false;
#else
  if (const char* sd = std::getenv("SteamDeck")) {
    if (std::strcmp(sd, "1") == 0) return true;
  }
  if (const char* sd2 = std::getenv("STEAM_DECK")) {
    if (std::strcmp(sd2, "1") == 0) return true;
  }
  return false;
#endif
}

DeviceStyle PromptDeviceMonitor::ClassifyGamepad(int sdl_type, uint16_t vid, uint16_t pid, std::string_view name,
                                                 bool steam_deck_env) const {
  const bool on_deck = steam_deck_env || IsSteamDeckEnvironment();

  // 1. Steam Deck: Valve VID 0x28DE + PID 0x1205, or "Steam Deck" in name, or "Steam Virtual Gamepad" on Deck.
  if ((vid == 0x28DE && pid == 0x1205) || ContainsCI(name, "steam deck") ||
      (on_deck && ContainsCI(name, "steam virtual gamepad"))) {
    return DeviceStyle::kSteamDeck;
  }

  // 2. Steam Controller: Valve VID 0x28DE + PID 0x1102 / 0x1142 / 0x1106, or "Steam Controller" in name.
  if ((vid == 0x28DE && (pid == 0x1102 || pid == 0x1142 || pid == 0x1106)) || ContainsCI(name, "steam controller")) {
    return DeviceStyle::kSteamController;
  }

  // 3. Nintendo Switch: SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO (7), JOYCON_LEFT (8), JOYCON_RIGHT (9), JOYCON_PAIR (10),
  // Nintendo VID 0x057E, or Switch/Joy-Con/Pro Controller name (when not Microsoft/Sony VID).
  if ((sdl_type >= 7 && sdl_type <= 10) || vid == 0x057E ||
      (vid != 0x045E && vid != 0x054C &&
       (ContainsCI(name, "switch") || ContainsCI(name, "joy-con") || ContainsCI(name, "joycon") ||
        ContainsCI(name, "pro controller")))) {
    return DeviceStyle::kSwitch;
  }

  // 4. PlayStation: SDL_GAMEPAD_TYPE_PS3 (4), PS4 (5), PS5 (6), Sony VID 0x054C, or DualShock/DualSense/PS name.
  if ((sdl_type >= 4 && sdl_type <= 6) || vid == 0x054C || ContainsCI(name, "dualshock") ||
      ContainsCI(name, "dualsense") || ContainsCI(name, "playstation") || ContainsCI(name, "ps4") ||
      ContainsCI(name, "ps5")) {
    return DeviceStyle::kPlayStation;
  }

  // 5. Steam Deck Environment Fallback: when running on Steam Deck and controller didn't match Switch/PS/SteamController.
  if (on_deck && (vid != 0x045E || steam_deck_env)) {
    return DeviceStyle::kSteamDeck;
  }

  // 6. Default controller profile: Xbox.
  return DeviceStyle::kXbox;
}

std::optional<DeviceStyle> PromptDeviceMonitor::ParseModeOption(std::string_view mode) {
  const std::string m = ToLowerTrim(mode);
  if (m.empty() || m == "auto") return std::nullopt;
  if (m == "xbox" || m == "xinput") return DeviceStyle::kXbox;
  if (m == "playstation" || m == "ps" || m == "ps4" || m == "ps5" || m == "dualsense" || m == "dualshock") {
    return DeviceStyle::kPlayStation;
  }
  if (m == "switch" || m == "nintendo" || m == "procontroller" || m == "joycon") return DeviceStyle::kSwitch;
  if (m == "steamdeck" || m == "deck" || m == "steam_deck") return DeviceStyle::kSteamDeck;
  if (m == "steamcontroller" || m == "steam" || m == "steam_controller") return DeviceStyle::kSteamController;
  if (m == "keyboard" || m == "kbm" || m == "mnk" || m == "mouse") return DeviceStyle::kKeyboard;
  if (m == "original" || m == "xbox360" || m == "off" || m == "none" || m == "default") return DeviceStyle::kOriginal;
  return std::nullopt;
}

std::optional<DeviceStyle> PromptDeviceMonitor::DetectConnectedRawInputPadStyle() {
#ifdef _WIN32
  UINT num_devices = 0;
  if (GetRawInputDeviceList(nullptr, &num_devices, sizeof(RAWINPUTDEVICELIST)) != 0 || num_devices == 0) {
    return std::nullopt;
  }
  std::vector<RAWINPUTDEVICELIST> raw_devices(num_devices);
  if (GetRawInputDeviceList(raw_devices.data(), &num_devices, sizeof(RAWINPUTDEVICELIST)) ==
      static_cast<UINT>(-1)) {
    return std::nullopt;
  }

  std::optional<DeviceStyle> found_style;
  for (UINT i = 0; i < num_devices; ++i) {
    if (raw_devices[i].dwType != RIM_TYPEHID) continue;
    UINT name_len = 0;
    GetRawInputDeviceInfoW(raw_devices[i].hDevice, RIDI_DEVICENAME, nullptr, &name_len);
    if (name_len == 0) continue;
    std::wstring dev_name(name_len, L'\0');
    if (GetRawInputDeviceInfoW(raw_devices[i].hDevice, RIDI_DEVICENAME, dev_name.data(), &name_len) ==
        static_cast<UINT>(-1)) {
      continue;
    }
    for (auto& ch : dev_name) {
      ch = static_cast<wchar_t>(towupper(ch));
    }

    // PlayStation: Sony VID 0x054C (DualSense, DualShock 4)
    if (dev_name.find(L"VID_054C") != std::wstring::npos) {
      return DeviceStyle::kPlayStation;
    }
    // Nintendo Switch: Nintendo VID 0x057E
    if (dev_name.find(L"VID_057E") != std::wstring::npos) {
      found_style = DeviceStyle::kSwitch;
    } else if (dev_name.find(L"VID_28DE") != std::wstring::npos) {
      // Valve Steam Deck / Steam Controller
      if (dev_name.find(L"PID_1205") != std::wstring::npos) {
        found_style = DeviceStyle::kSteamDeck;
      } else if (!found_style) {
        found_style = DeviceStyle::kSteamController;
      }
    } else if (dev_name.find(L"VID_045E") != std::wstring::npos && !found_style) {
      found_style = DeviceStyle::kXbox;
    }
  }
  return found_style;
#else
  return std::nullopt;
#endif
}

std::optional<DeviceStyle> PromptDeviceMonitor::ScanConnectedPadStyle() const {
  if (auto raw = DetectConnectedRawInputPadStyle()) return raw;
#ifdef _WIN32
  static SdlGamepadApi sdl;  // monitor thread only
  if (sdl.Resolve()) {
    int count = 0;
    SdlGamepadApi::SDL_JoystickID* ids = sdl.GetGamepads(&count);
    std::optional<DeviceStyle> found;
    for (int i = 0; ids && i < count && !found; ++i) {
      const auto id = ids[i];
      const int type = sdl.GetGamepadTypeForID ? sdl.GetGamepadTypeForID(id) : 0;
      const uint16_t vid = sdl.GetGamepadVendorForID ? sdl.GetGamepadVendorForID(id) : 0;
      const uint16_t pid = sdl.GetGamepadProductForID ? sdl.GetGamepadProductForID(id) : 0;
      const char* name = sdl.GetGamepadNameForID ? sdl.GetGamepadNameForID(id) : nullptr;
      found = ClassifyGamepad(type, vid, pid, name ? name : "");
    }
    if (ids) sdl.Free(ids);
    if (found) return found;
  }
#endif
  if (IsSteamDeckEnvironment()) return DeviceStyle::kSteamDeck;
  return std::nullopt;
}

void PromptDeviceMonitor::AttachInputSystem(rex::input::InputSystem* input_system) {
  attached_input_system_.store(input_system, std::memory_order_release);
}

void PromptDeviceMonitor::DetachInputSystem(rex::input::InputSystem* input_system) {
  rex::input::InputSystem* expected = input_system;
  if (!input_system) {
    attached_input_system_.store(nullptr, std::memory_order_release);
  } else {
    attached_input_system_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
  }
}

void PromptDeviceMonitor::ReadModeLocked() {
  mode_ = Mode::kAuto;
  forced_style_ = DeviceStyle::kOriginal;
  const std::string values[] = {REXCVAR_GET(gow_button_prompts), REXCVAR_GET(gow2_button_prompts),
                                rex::cvar::GetFlagByName("gow3_button_prompts"),
                                rex::cvar::GetFlagByName("gowj_button_prompts")};
  for (const std::string& v : values) {
    if (const auto style = ParseModeOption(v)) {
      mode_ = *style == DeviceStyle::kOriginal ? Mode::kOriginal : Mode::kForced;
      forced_style_ = *style;
      break;
    }
  }
  keyboard_allowed_ = !REXCVAR_GET(gow_button_prompts_keep_pad);
}

void PromptDeviceMonitor::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_kbm_ms_ = 0;
  last_pad_ms_ = 0;
  has_detected_pad_ = false;
  last_detected_pad_style_ = DeviceStyle::kXbox;
  committed_auto_style_ = DeviceStyle::kXbox;
  has_committed_auto_ = false;
  has_applied_style_ = false;
  last_keybind_sig_.clear();
  keys_down_.reset();
  mouse_down_ = false;
  ReadModeLocked();
}

void PromptDeviceMonitor::ResetInputStates() {
  std::lock_guard<std::mutex> lock(mutex_);
  keys_down_.reset();
  mouse_down_ = false;
}

void PromptDeviceMonitor::NotifyKeyDown(uint64_t timestamp_ms) { NotifyKeyboardMouseActivity(timestamp_ms); }

void PromptDeviceMonitor::NotifyKeyDown(uint32_t vk, uint64_t timestamp_ms) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vk < 256) keys_down_.set(vk);
  }
  NotifyKeyboardMouseActivity(timestamp_ms);
}

void PromptDeviceMonitor::NotifyKeyUp(uint64_t) {
  std::lock_guard<std::mutex> lock(mutex_);
  keys_down_.reset();
}

void PromptDeviceMonitor::NotifyKeyUp(uint32_t vk, uint64_t) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (vk < 256) keys_down_.reset(vk);
}

void PromptDeviceMonitor::NotifyMouseDown(uint64_t timestamp_ms) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mouse_down_ = true;
  }
  NotifyKeyboardMouseActivity(timestamp_ms);
}

void PromptDeviceMonitor::NotifyMouseUp(uint64_t) {
  std::lock_guard<std::mutex> lock(mutex_);
  mouse_down_ = false;
}

bool PromptDeviceMonitor::HasAnyKeyDownLocked(bool check_async) const {
  if (!keys_down_.any()) return false;
#ifdef _WIN32
  if (!check_async) return false;  // not in front: a key held elsewhere is not ours (and its key-up never reaches us)
  bool really_down = false;
  for (size_t i = 0; i < 256; ++i) {
    if (!keys_down_.test(i)) continue;
    if ((GetAsyncKeyState(static_cast<int>(i)) & 0x8000) != 0) {
      really_down = true;
    } else {
      keys_down_.reset(i);  // a key-up we missed
    }
  }
  return really_down;
#else
  (void)check_async;
  return true;
#endif
}

bool PromptDeviceMonitor::IsKeyboardMouseInUse(uint64_t now_ms) const {
  if (now_ms == 0) now_ms = NowMs();
#ifdef _WIN32
  const bool in_front = GameWindowInFront();
  if (in_front && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                   (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0)) {
    return true;
  }
  std::lock_guard<std::mutex> lock(mutex_);
#else
  const bool in_front = true;
  std::lock_guard<std::mutex> lock(mutex_);
  if (mouse_down_) return true;
#endif
  if (HasAnyKeyDownLocked(in_front)) return true;
  return last_kbm_ms_ != 0 && now_ms < last_kbm_ms_ + kKbmRecentMs;
}

void PromptDeviceMonitor::NotifyKeyboardMouseActivity(uint64_t timestamp_ms) {
  if (timestamp_ms == 0) timestamp_ms = NowMs();
  std::lock_guard<std::mutex> lock(mutex_);
  last_kbm_ms_ = timestamp_ms;
  if (!has_committed_auto_) {
    committed_auto_style_ = DeviceStyle::kKeyboard;
    has_committed_auto_ = true;
  } else if (committed_auto_style_ != DeviceStyle::kKeyboard) {
    if (last_pad_ms_ == 0 || timestamp_ms >= last_pad_ms_ + kDebounceWindowMs) {
      committed_auto_style_ = DeviceStyle::kKeyboard;
    }
  }
}

void PromptDeviceMonitor::NotifyMouseMove(int32_t dx, int32_t dy, uint64_t timestamp_ms) {
  if (std::abs(dx) <= kMouseMoveDeadzonePx && std::abs(dy) <= kMouseMoveDeadzonePx) return;
  NotifyKeyboardMouseActivity(timestamp_ms);
}

void PromptDeviceMonitor::NotifyGamepadActivity(DeviceStyle pad_style, uint64_t timestamp_ms) {
  if (timestamp_ms == 0) timestamp_ms = NowMs();
  std::lock_guard<std::mutex> lock(mutex_);
  last_detected_pad_style_ = pad_style;
  has_detected_pad_ = true;
  last_pad_ms_ = timestamp_ms;
  if (!has_committed_auto_ || committed_auto_style_ != DeviceStyle::kKeyboard || last_kbm_ms_ == 0 ||
      timestamp_ms >= last_kbm_ms_ + kDebounceWindowMs) {
    committed_auto_style_ = pad_style;
    has_committed_auto_ = true;
  }
}

void PromptDeviceMonitor::SetDefaultGamepadStyle(DeviceStyle pad_style) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (last_pad_ms_ != 0 && has_detected_pad_) return;  // a pad was used: its style stands
  last_detected_pad_style_ = pad_style;
  has_detected_pad_ = true;
  if (!has_committed_auto_ || committed_auto_style_ != DeviceStyle::kKeyboard) {
    committed_auto_style_ = pad_style;
  }
}

DeviceStyle PromptDeviceMonitor::GetDefaultGamepadStyle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_detected_pad_ ? last_detected_pad_style_ : DeviceStyle::kXbox;
}

DeviceStyle PromptDeviceMonitor::EvaluateActiveStyleLocked(uint64_t now_ms) {
  if (mode_ == Mode::kOriginal) return DeviceStyle::kOriginal;
  if (mode_ == Mode::kForced) return forced_style_;

  DeviceStyle style;
  if (last_kbm_ms_ == 0 && last_pad_ms_ == 0) {
    // Nothing pressed yet: the pad that is connected, else key caps when the game is set for keyboard and mouse.
    if (has_detected_pad_) {
      style = last_detected_pad_style_;
    } else if (keyboard_allowed_ && rex::cvar::Query<bool>("mnk_mode")) {
      style = DeviceStyle::kKeyboard;
    } else {
      style = DeviceStyle::kXbox;
    }
  } else {
    const uint64_t eval_ms = now_ms != 0 ? now_ms : std::max(last_kbm_ms_, last_pad_ms_);
    if (!has_committed_auto_) {
      committed_auto_style_ = last_kbm_ms_ > last_pad_ms_ ? DeviceStyle::kKeyboard : last_detected_pad_style_;
      has_committed_auto_ = true;
    } else if (committed_auto_style_ == DeviceStyle::kKeyboard && last_pad_ms_ > last_kbm_ms_) {
      if (last_kbm_ms_ == 0 || eval_ms >= last_kbm_ms_ + kDebounceWindowMs) {
        committed_auto_style_ = last_detected_pad_style_;
      }
    } else if (committed_auto_style_ != DeviceStyle::kKeyboard && last_kbm_ms_ > last_pad_ms_) {
      if (last_pad_ms_ == 0 || eval_ms >= last_pad_ms_ + kDebounceWindowMs) {
        committed_auto_style_ = DeviceStyle::kKeyboard;
      }
    } else if (committed_auto_style_ != DeviceStyle::kKeyboard) {
      committed_auto_style_ = last_detected_pad_style_;
    }
    style = committed_auto_style_;
  }
  // GitHub #104: with "Controller button prompts" on, keyboard / mouse use never brings the key caps.
  if (style == DeviceStyle::kKeyboard && !keyboard_allowed_) {
    style = has_detected_pad_ ? last_detected_pad_style_ : DeviceStyle::kXbox;
  }
  // Auto: an Xbox pad shows the game's own (Xbox 360) prompts - no overlay, no atlas.
  if (style == DeviceStyle::kXbox) style = DeviceStyle::kOriginal;
  return style;
}

DeviceStyle PromptDeviceMonitor::EvaluateActiveStyle(uint64_t now_ms) {
  auto& compositor = ::xenos::renderer::prompt::GlobalPromptCompositor();
  DeviceStyle target;
  bool hide = false;
  bool apply = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    target = EvaluateActiveStyleLocked(now_ms);
    hide = mode_ == Mode::kAuto && target == DeviceStyle::kOriginal;
    if (!has_applied_style_ || last_applied_style_ != target || last_applied_hide_ != hide) {
      if (has_applied_style_) {
        REXLOG_INFO("prompt_device_monitor: {} -> {}", DeviceStyleName(last_applied_style_), DeviceStyleName(target));
      }
      last_applied_style_ = target;
      last_applied_hide_ = hide;
      has_applied_style_ = true;
      apply = true;
    }
  }
  // Another monitor (tests) may have moved the compositor; follow it back.
  if (!apply && compositor.ActiveStyle() != target) apply = true;
  // Outside our lock: building a style's atlases takes a while and window events must not wait on it.
  if (apply) {
    // Explicit "original" never touches the texture cache's overlays at all (the launcher's key caps stay as before).
    bool explicit_original = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      explicit_original = mode_ == Mode::kOriginal;
    }
    if (!explicit_original || compositor.ActiveStyle() != DeviceStyle::kOriginal) {
      compositor.SetActiveStyle(target, hide);
    }
  }
  published_style_.store(target, std::memory_order_relaxed);
  return target;
}

std::string PromptDeviceMonitor::SnapshotKeybindSignature() const {
  std::string sig;
  sig.reserve(256);
  for (const char* k : kKeybindCvars) {
    sig += rex::cvar::GetFlagByName(k);
    sig.push_back('|');
  }
  return sig;
}

DeviceStyle PromptDeviceMonitor::Poll(uint64_t now_ms, bool) {
  if (now_ms == 0) now_ms = NowMs();
  bool rebuild = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ReadModeLocked();
    std::string sig = SnapshotKeybindSignature();
    rebuild = !last_keybind_sig_.empty() && sig != last_keybind_sig_;
    last_keybind_sig_ = std::move(sig);
  }
  if (rebuild) ::xenos::renderer::prompt::GlobalPromptCompositor().RebuildKeyboardOverlays();
  return EvaluateActiveStyle(now_ms);
}

void PromptDeviceMonitor::Start(rex::ui::Window* window, rex::input::InputSystem* input_system) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ReadModeLocked();
    if (mode_ == Mode::kOriginal) {
      REXLOG_INFO("prompt_device_monitor: --gow_button_prompts=original - the game's own prompts, nothing started");
      return;
    }
  }
  if (thread_.joinable()) return;
  AttachInputSystem(input_system);
  AttachWindow(window);
  stop_.store(false);
  thread_ = std::thread([this] { ThreadMain(); });
}

void PromptDeviceMonitor::Stop() {
  if (!thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_.store(true);
  }
  stop_cv_.notify_all();
  thread_.join();
  DetachInputSystem(nullptr);
}

void PromptDeviceMonitor::ThreadMain() {
  auto& compositor = ::xenos::renderer::prompt::GlobalPromptCompositor();
  if (!compositor.IsInitialized() && !compositor.Initialize()) {
    REXLOG_WARN("prompt_device_monitor: no glyphs found - the game's own prompts stay");
    return;
  }
  std::optional<DeviceStyle> scanned;
  uint64_t next_scan = 0;
  int16_t prev[4] = {};
  bool has_prev = false;
  while (!stop_.load()) {
    const uint64_t now = NowMs();

    // Every 2 s: which pad is connected (slow: RawInput / SDL lists), build the atlases the switch may need next so
    // switching is instant, and follow keybind changes.
    if (now >= next_scan) {
      next_scan = now + kHardwareScanMs;
      scanned = ScanConnectedPadStyle();
      if (scanned) SetDefaultGamepadStyle(*scanned);
      Mode mode;
      DeviceStyle forced;
      bool kb_allowed;
      bool rebuild = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        mode = mode_;
        forced = forced_style_;
        kb_allowed = keyboard_allowed_;
        std::string sig = SnapshotKeybindSignature();
        rebuild = !last_keybind_sig_.empty() && sig != last_keybind_sig_;
        last_keybind_sig_ = std::move(sig);
      }
      if (rebuild) compositor.RebuildKeyboardOverlays();
      if (mode == Mode::kForced) {
        compositor.PrepareStyle(forced);
      } else {
        if (kb_allowed) compositor.PrepareStyle(DeviceStyle::kKeyboard);
        const DeviceStyle pad = GetDefaultGamepadStyle();
        if (pad != DeviceStyle::kXbox) compositor.PrepareStyle(pad);
      }
    }

    // Pads: the game's own input system (SDL: Xbox, PlayStation, Switch, Steam pads), plain XInput when there is none.
    rex::input::InputSystem* input = attached_input_system_.load(std::memory_order_acquire);
    if (!input) {
      if (auto* rt = rex::Runtime::instance()) {
        input = static_cast<rex::input::InputSystem*>(rt->input_system());
        if (input) AttachInputSystem(input);
      }
    }
    bool connected = false;
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t sticks[4] = {};
    if (input) {
      for (uint32_t i = 0; i < 4 && !connected; ++i) {
        rex::input::X_INPUT_STATE st = {};
        if (input->GetStateForUI(i, &st) == rex::X_RESULT(0)) {  // X_ERROR_SUCCESS
          buttons = static_cast<uint16_t>(st.gamepad.buttons);
          lt = st.gamepad.left_trigger;
          rt = st.gamepad.right_trigger;
          sticks[0] = static_cast<int16_t>(st.gamepad.thumb_lx);
          sticks[1] = static_cast<int16_t>(st.gamepad.thumb_ly);
          sticks[2] = static_cast<int16_t>(st.gamepad.thumb_rx);
          sticks[3] = static_cast<int16_t>(st.gamepad.thumb_ry);
          connected = true;
        }
      }
    }
#ifdef _WIN32
    else if (XInputGetStateFn get = ResolveXInputGetState()) {
      for (DWORD i = 0; i < 4 && !connected; ++i) {
        XINPUT_STATE s = {};
        if (get(i, &s) == ERROR_SUCCESS) {
          buttons = s.Gamepad.wButtons;
          lt = s.Gamepad.bLeftTrigger;
          rt = s.Gamepad.bRightTrigger;
          sticks[0] = s.Gamepad.sThumbLX;
          sticks[1] = s.Gamepad.sThumbLY;
          sticks[2] = s.Gamepad.sThumbRX;
          sticks[3] = s.Gamepad.sThumbRY;
          connected = true;
        }
      }
    }
#endif
    if (connected) {
      // A pad the keyboard drives (Steam Input sending keys as a pad) is ignored while keys / mouse are in use; resting
      // stick drift counts only when it moves.
      const bool kbm_in_use = IsKeyboardMouseInUse(now);
      bool active = !kbm_in_use && (buttons != 0 || lt > 40 || rt > 40);
      if (!active && !kbm_in_use) {
        constexpr int kStickDeadzone = 14000;
        for (int a = 0; a < 4 && !active; ++a) {
          const int v = std::abs(static_cast<int>(sticks[a]));
          if (v <= kStickDeadzone) continue;
          active = !has_prev || v > 20000 || std::abs(static_cast<int>(sticks[a]) - prev[a]) > 2000;
        }
      }
      std::copy(std::begin(sticks), std::end(sticks), std::begin(prev));
      has_prev = true;
      if (active) NotifyGamepadActivity(scanned.value_or(GetDefaultGamepadStyle()), now);
    }

    EvaluateActiveStyle(now);

    std::unique_lock<std::mutex> lock(mutex_);
    stop_cv_.wait_for(lock, std::chrono::milliseconds(kPadPollMs), [this] { return stop_.load(); });
  }
}

void PromptDeviceMonitor::AttachWindow(rex::ui::Window* window) {
  if (!window) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (attached_window_ == window) return;
  if (attached_window_ && window_watcher_) {
    attached_window_->RemoveInputListener(window_watcher_.get());
    attached_window_->RemoveListener(window_watcher_.get());
  }
  if (!window_watcher_) window_watcher_ = std::make_unique<WindowWatcher>(*this);
  attached_window_ = window;
  attached_window_->AddInputListener(window_watcher_.get(), 0);
  attached_window_->AddListener(window_watcher_.get());
}

void PromptDeviceMonitor::DetachWindow(rex::ui::Window* window) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!attached_window_) return;
  if (window != nullptr && window != attached_window_) return;
  if (window_watcher_) {
    attached_window_->RemoveInputListener(window_watcher_.get());
    attached_window_->RemoveListener(window_watcher_.get());
  }
  attached_window_ = nullptr;
}

PromptDeviceMonitor& GlobalPromptDeviceMonitor() {
  static PromptDeviceMonitor instance;
  return instance;
}

}  // namespace xenos::input
