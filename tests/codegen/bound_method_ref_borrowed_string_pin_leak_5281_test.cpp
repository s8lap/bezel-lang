// fix-forward #5281 (§4.6 / §28.9, v0.313): the borrowed-string-argument pin
// is missing from the BOUND METHOD-REFERENCE invocation path (emitClosureCall).
// The pin is emitted for a borrowed by-value `string` / `string?` argument before
// the call (the caller retains to keep the string alive for the callee's borrow),
// but `emitClosureCall` had no `releaseBorrowedStringArgPins` on the normal-return
// edge. The direct method call path (`lowerMethodCall`) was fixed by #5244/#5262;
// the closure call path was missed.
//
// THE SHAPE: `var f := c.Twice; f(c.Val, c.Val);` where `Twice` is an instance
// method with `string` parameters and `c.Val` is a borrowed string (not a literal,
// not a +1 producer). The method stores the parameters in nullable fields,
// overwriting with the same value. The caller's pin retains each argument for the
// callee's borrow; without the fix, the pin is never released on the normal-return
// edge, leaking one string pair per call. Loop-amplified 200x.
//
// SIBLING FIX #5280 covers the ASYNC over-early release on an async path.
// This row covers the UNDER-release on the normal-return path (a leak, the safe
// direction — the orphaning variant that would UAF is not pin-able as a row).
//
// Spec: §4.6 "`string`" and §28.9 "Runtime-managed types" — the
// reference-counting contract is that a parameter naming a `string` value keeps
// reading that value for the whole call. Spec revision v0.313.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The two shapes #5281 filed:
//   Shape 1: THROWING CALLEE, method-call arm — already covered by
//     borrowed_string_arg_pin_5280_5281_test.cpp:2a, which pins the
//     reachability form of that arm (the orphaning form is already pinned
//     by method_call_arm_orphan_pin_5262_test.cpp).
//   Shape 2: BOUND METHOD-REFERENCE call — tested here. The method stores
//     each parameter in a nullable field, then overwrites with the same value
//     (releasing the first store). The pin retains the arguments for the
//     callee's borrow; without the fix, the pin is never released.

std::string twiceMethodRefSrc() {
    return "module M; "
           "class C "
           "  public Val: string := \"hello\"; "
           "  private _a: string? := null; "
           "  private _b: string? := null; "
           "  public method Twice(a: string, b: string): int "
           "    _a := a; _b := b; "   // first store: retains a, b
           "    _a := a; _b := b; "   // overwrite: releases old, retains new
           "    return 0; "
           "  end Twice; "
           "  public static method Main(): int "
           "    var c := new C(); "
           "    var f := c.Twice; "  // bound method reference
           "    var i := 0; "
           "    while i < 200 do "
           "      f(c.Val, c.Val); "  // call via bound method reference
           "      i := i + 1; "
           "    end; "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// Control: same program but calling Twice DIRECTLY — goes through
// lowerMethodCall which already has the pin. Should be clean with and
// without the fix.
std::string twiceDirectCallSrc() {
    return "module M; "
           "class C "
           "  public Val: string := \"hello\"; "
           "  private _a: string? := null; "
           "  private _b: string? := null; "
           "  public method Twice(a: string, b: string): int "
           "    _a := a; _b := b; "
           "    _a := a; _b := b; "
           "    return 0; "
           "  end Twice; "
           "  public static method Main(): int "
           "    var c := new C(); "
           "    var i := 0; "
           "    while i < 200 do "
           "      c.Twice(c.Val, c.Val); "  // direct call
           "      i := i + 1; "
           "    end; "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// #5281 SHAPE 2 — BOUND METHOD-REFERENCE CALL
// ═══════════════════════════════════════════════════════════════════════════

// (main row) Bound method reference with borrowed string arguments.
// The method stores each parameter twice (second store releases the first).
// The caller's pin retains the arguments during the call; without the fix,
// the pin is never released on the normal-return edge, leaking one string
// pair per call. Loop-amplified 200x to make the leak unmistakable to LSan.
// With the fix, the pin is released after the call and no leak occurs.
BEZEL_TEST(bound_method_ref_borrowed_string_pin_leak_5281,
           bound_method_ref_twice_string_pin_no_leak_200x) {
    auto r = runAot(twiceMethodRefSrc());
    // Plain runAot: no sanitizer, just program logic. The program exits 0
    // if and only if the borrowed strings were correctly retained during the
    // call. A UAF would cause a crash or wrong output before this check.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (control) Same program with a direct call instead of a bound method ref.
// This goes through lowerMethodCall which already has the pin release.
// Should be clean with and without the fix.
BEZEL_TEST(bound_method_ref_borrowed_string_pin_leak_5281,
           direct_call_twice_string_pin_no_leak_control) {
    auto r = runAot(twiceDirectCallSrc());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
