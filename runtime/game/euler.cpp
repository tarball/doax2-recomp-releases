// The Pool's air mattress stood upright in some frames (DOAX2-specific addresses).
//
// sub_825A57F8 updates a prop that follows a girl's motion (the air mattress Kokoro lies on at the Pool): it samples the
// prop's rotation from the motion's keys as a quaternion (sub_825A4E78), converts it to Euler angles, and builds the
// prop's world matrix from them. The conversion computes m22 = 1 - 2(x^2 + y^2) and m11 = 1 - 2(x^2 + z^2) and normally
// takes Y = atan2(-2(wy + xz), m22) and Z = atan2(-2(xy + wz), m11), but when m22 (0x825A5AF4) or m11 (0x825A5B74) is
// exactly 0.0 it takes special cases built from acos and asin that give a different rotation (Euler X and Y of 90
// degrees), which stood the mattress on end (its reflection too). The mattress's keys are Euler angles of about (90, 85,
// 90) degrees, so its quaternions are (a, b, a, b) and m22 is 0 in exact arithmetic: whether a^2 + b^2 comes out as
// exactly 0.5 in single precision rests on the last bits of the sampler's quaternion products, VMX128 dot products
// (vmsum4fp128) of two nonzero terms each. Rounded to nearest (XenonRecomp's dpps, and Xenia Canary's float64 sum
// alike) 52 or 53 of the scene's frames take the special case (one in four while she lies on it); rounded toward zero,
// none. Xenon's rounding there isn't documented, and a flicker that often would hardly have passed the game's testing,
// so the console most likely rounds otherwise and never takes it (to be checked on hardware). Everything else on the way
// matches the console: the keys, the motion clock (1 + 1e-6 per frame), the sines and cosines (correctly rounded) and
// the conversion's IEEE arithmetic.
//
// The mid-asm hooks (config/DOAX2.toml) send those cases to the conversion's general formulas whenever the atan2's
// numerator isn't 0 (atan2 is well defined there, and the general formulas give the rotation the frames around them
// have), keeping the game's special cases for a 0 numerator too (gimbal lock). DEBUG_EULER_SPECIAL_CASES=1 keeps the
// game's behavior (diagnostics).
#include "guest_abi.h"

#include <cstdlib>

// At sub_825A57F8's tests of m22 (f30, numerator f27) and m11 (f29, numerator f28) against 0.0: true skips the special
// cases for that axis.
bool EulerZeroDenominatorMidAsmHook(PPCRegister& denominator, PPCRegister& numerator)
{
    static const bool original = [] {
        const char* setting = std::getenv(GAME_PREFIX "_DEBUG_EULER_SPECIAL_CASES");
        return setting && std::atoi(setting) != 0;
    }();
    return !original && denominator.f64 == 0.0 && numerator.f64 != 0.0;
}
