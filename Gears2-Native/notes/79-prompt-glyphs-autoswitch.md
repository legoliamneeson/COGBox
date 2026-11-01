# 79 - Multi-Controller Prompt Glyphs and Live Auto-Switching

Decided and implemented for CogBox (Gears 2, Gears 3, Gears of War: Judgment).

## Summary

Native PC ports historically displayed hardcoded Xbox 360 controller button prompts across HUD action prompts, menus, and chainsaw duels, or forced static keyboard prompts.

This change adds:
1. High-resolution (4x) vector-sourced button prompt glyphs bundled from Kenney Input Prompts (CC0 1.0 Public Domain).
2. Live hardware auto-switching between Keyboard & Mouse, Xbox, PlayStation (DualSense / DualShock), Nintendo Switch, Steam Deck, and Steam Controller based on the active input device.
3. Configurable manual overrides via `--gow_button_prompts=auto|xbox|playstation|switch|steamdeck|steamcontroller|keyboard|original`.

### Modes (rework of PR #117)
- `auto` (the default): an Xbox / XInput pad shows the **game's own** prompts (no overlay, no atlas built);
  PlayStation, Switch, Steam Deck and Steam Controller pads get their Kenney glyphs; keyboard / mouse gets the key caps.
  Switching is instant both ways: the atlases auto may need next (key caps, and the connected non-Xbox pad's) are built
  in advance on the monitor thread, so a switch only swaps overlay pointers.
- GitHub #104: with the launcher's "Controller button prompts" option (Keyboard and mouse on), the launcher passes
  `--gow_button_prompts_keep_pad=true` and auto never switches to key caps.
- `original`: the monitor never starts and nothing is drawn over the game's prompts (exactly the behaviour before this
  feature, including the launcher's own key-cap overlays).
- Any other value forces that style.

---

## Architecture & Layer 0 Implementation

All logic resides in the shared engine foundation `Gears2-Native/src/layer0_xenos`:

### 1. `prompt_device_monitor` (`xenos::input::PromptDeviceMonitor`)
- Tracks active input devices across SDL3 gamepads and Win32 RawInput / mouse hooks.
- **Stick Drift & Deadzone Filtering**: Ignores resting stick drift below `0.18f` and trigger resting states so plugged-in controllers do not steal focus from keyboard/mouse.
- **Mouse Warp & Lockout Reversion**: Suppresses cursor-centering synthetic mouse moves (e.g. during camera capture or menu centering) from falsely triggering keyboard/mouse mode.
- **Hysteresis / Debounce**: Applies a 250 ms debounce window (`kDebounceWindowMs`) before switching input styles to prevent rapid flapping when both devices are touched.
- Keyboard / mouse only counts while the game window is in front (held keys and buttons are checked with
  `GetAsyncKeyState` only then), so clicks in other apps do not flip the prompts and the pad keeps working in the
  background (SDK patch 53).
- **Threads**: everything slow - pad reads (InputSystem / XInput, every 16 ms), the RawInput / SDL device scan (every
  2 s; SDL is only listed, no gamepad is opened) and building atlases - runs on the monitor's own thread
  (`PromptDeviceMonitor::Start` / `Stop`, from the in-game menu install / shutdown, menu or no menu). The render thread
  only does one atomic load (`PublishedStyle()`, which `TextureCache::Bind`'s `PollFromBind()` call now is).

### 2. `prompt_overlay_compositor` (`xenos::renderer::PromptOverlayCompositor`)
- Hooks into texture creation and upload in the Xenos renderer pipeline (`texture_cache.cpp`).
- When a target font/HUD atlas texture is bound, composites high-res glyph cells into a 4x host texture layer.
- A style's four 4x atlases are built the first time it is needed; at most two styles stay resident (the rest are
  dropped, or taken back while the texture cache still holds them).
- **Font Quad Margin Scaling (0.85x)**: UE3 font character quads in atlas `A52EE00B` clip tight against the UV cell edge at `Y = 55.5` (cell top `Y = 54.0`). Glyphs are scaled by `0.85f` centered within each cell, leaving a safe 2px margin at 1x (8px at 4x) to avoid flat/clipped button tops in menus and dialogues.

### 3. `prompt_atlas_catalog` (`xenos::renderer::PromptAtlasCatalog`)
- Defines known texture hashes for HUD prompt sheets, font sheets, and chainsaw duel quick-time events across Gears 2, Gears 3, and Judgment.
- Maps UV bounding boxes for A, B, X, Y, LB, RB, LT, RT, LS, RS, D-pad, Start, and Back.

### 4. Integration with Runtime Input Polling
- The monitor thread reads the pads itself (the in-game menu's `PollPads` is unchanged).

---

## Testing & Verification
- Unit tests in `gn_tests.exe` (the contributor's run of the original PR: 123 tests; the rework's tests are not run yet).
- Verified in-game across:
  - Gears of War 2 (Disc US & TU6)
  - Gears of War 3 (TU6)
  - Gears of War: Judgment (Disc & TU4)
- Verified seamless switching between DualSense controller and Keyboard & Mouse in menus, gameplay, and weapon switch prompts.

