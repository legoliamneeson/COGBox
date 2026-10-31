// COGBox 0.2.1: native mouse look for Gears of War (2006) - its build's addresses (see ue3_mouse_look.h).
// {name, gears2 way (ProcessInput), entry function, player axis (unused), friction, MouseX name, MouseY name,
//  RightX name, RightY name, check slot, check function, client offset, viewport argument offset, XInput state wrapper}
// GitHub #82: Gears 1 still drove the right stick from the mouse (capped / slow turning).
// Addresses read from Gears 1 Title Update 5's generated code and a loaded image (see the g1-mouse report):
//  - ProcessInput 0x82988948 (vtable slot 0x8215CC98): XInput state per pad, then the viewport client's InputAxis
//    (vtable +24) with XboxTypeS_LeftX/LeftY/RightX/RightY; client at this +56, viewport argument this +4, no bGamepad.
//  - Key names from the name set-up sub_822960F8 (L"MouseX" 0x820D1790 ... L"XboxTypeS_RightY" 0x820D1D78).
//  - Friction: AWarPC::execGetFrictionTarget 0x821DB478 (native table entry 0x82F9DA98).
//  - Pad used last: the XInput state wrapper sub_829F4448 (-> XamInputGetState), called by ProcessInput.
#include "ue3_mouse_look.h"
#include "mouse_look.h"

namespace {
constexpr ue3_mouse::Build kBuild = {
    "Gears of War (Title Update 5)", true, 0x82988948, 0, 0x821DB478,
    0x82FDCD74, 0x82FDCD7C, 0x82FDD0D4, 0x82FDD0DC,
    0x8215CC98, 0x82988948,
    56, 4, 0x829F4448};
}  // namespace

void CogMouseLookInstall(rex::ui::Window* window) { ue3_mouse::Install(kBuild, window); }
