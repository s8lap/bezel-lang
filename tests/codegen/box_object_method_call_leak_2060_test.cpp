// Codegen / runtime — §8.6 / §28.9 (#2060): a boxed value type assigned to an
// `Object`-typed local that is read by a DIRECT METHOD CALL on the local
// (`o.GetHashCode()`, `o.Equals(...)`, `o.ToString()`) must have its box wrapper
// released at scope end. The conservative escape walker (BoxEscapeWalker) used
// to treat ANY member access on the box as an escape — including a direct
// method-call receiver — so the box was left unreached by `bz_box_release` and
// leaked at scope end.
//
// §8.6 (v0.204): "When a value type is used where `Object` (or an interface
// implemented by the value type) is expected, the value is boxed: a
// heap-allocated wrapper holds the value. Boxed values are runtime-managed
// (§28.9)."
//
// §28.9: "Boxed value types ... are exempt from manual disposal ... the type's
// lifetime is managed by the runtime without user involvement."
//
// §22.7 / §28.3 (v0.184): inside a method, `this` is a BORROW — a method body
// cannot retain, transfer, or dispose its borrowed receiver, and Bezel's
// ownership rules forbid storing a borrow into a longer-lived location. So a
// direct method call on the box cannot cause the box to be retained. This
// matches codegen reality: `bz_box_retain` is emitted ONLY for lambda capture
// and class-field store (LlvmBackend_Exprs2.cpp / LlvmBackend_Setup.cpp:5421),
// never for a plain method-call receiver. Treating the call receiver as a safe
// read is therefore sound: the box's rc is unchanged by the call, the scope-end
// release drops rc 1→0 and frees the wrapper exactly once.
//
// THE BUG (#2060). For `var o: Object := <boxed value>; o.GetHashCode();` the
// escape walker's CallExpr handler recursed into the callee (a MemberAccessExpr)
// with safeRead=false, which then recursed into the receiver `o` as an escape.
// The local was therefore NOT in boxReleaseEligible_, no scope-end
// `bz_box_release` was emitted, and every iteration of a loop leaked the
// 24-byte box wrapper. Discovered during the #2058 ASan probe.
//
// THE FIX. BoxEscapeWalker's CallExpr / NullCondCallExpr handlers now detect
// when the callee is a MemberAccessExpr / NullCondMemberExpr whose target IS
// the tracked local: the receiver is treated as a safe read (skip the descent),
// matching the spec's borrowed-`this` rule and the codegen retain surface. The
// call's ARGS still escape (passing the box itself to another method could
// still be a retain). A bare member access (`var f := o.GetHashCode;` — a
// method reference) is still an escape, since the closure captures `o`.
//
// OBSERVABILITY. The leak is a DIRECT leak (no other reference reaches the
// box), so runAotChecked's LSan flags it: WITHOUT the fix `r.ran` is false;
// WITH it the run is clean AND no double-free / use-after-free is introduced
// (the dispose-exactly-once net rides on `r.ran`).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// §8.6 / §28.9 / #2060: the issue's exact pattern. A record `r` boxed into
// `o: Object`, then `o.GetHashCode()` is called as a direct method call on the
// box. 100 iterations amplify the leak (2400 bytes total at 24 bytes/box) so
// LSan catches it decisively. The exit code is `sink mod 256` — the program's
// own observable value (independent of the leak). WITHOUT the fix LSan flips
// r.ran false; WITH it the run is clean.
const char* kBoxObjectMethodCallLoop =
    "module M; "
    "record Rec public x: int; public constructor(x: int) this.x := x; "
    "  end constructor; end Rec; "
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 100 do "
    "    var r := new Rec(j); "
    "    var o: Object := r; "             // BOX → must be released each iter
    "    sink := sink + o.GetHashCode(); " // direct method call on the box
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

// §8.6 / §28.9 / #2060: variant — a boxed primitive (int) read by
// `o.GetHashCode()` and `o.Equals(null)`. Two direct method calls on the same
// box per iteration. Both receivers must be safe reads; both args (the `null`)
// are walked as escapes but are not the tracked local. WITHOUT the fix each
// iteration's int-box leaks.
const char* kBoxIntMethodCallLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var j := 0; var sink := 0; "
    "    while j < 100 do "
    "      var o: Object := j; "               // BOX int → release each iter
    "      sink := sink + o.GetHashCode(); "   // direct method call #1
    "      if o.Equals(null) then sink := sink + 1; end; " // #2 (always false)
    "      j := j + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// §8.6 / §28.9 / #2060: variant — ToString on the box returns a string (which
// has its OWN pre-existing leak path when stored in an inferred local —
// `var s := r.ToString();` leaks the same way without any boxing — so this
// variant is omitted here to keep the test #2060-specific). The box itself,
// when read by `o.ToString()` directly inside a larger expression, is released
// correctly; that case is covered indirectly by the GetHashCode variants above
// (the receiver rule is identical).

// §8.6 / §28.9 / #2060: variant — null-conditional method call `o?.GetHashCode()`
// (§7.5). The NullCondCallExpr path must relax the same way as CallExpr. Here
// `o` is non-nullable Object (always set), so the call always dispatches; the
// box must be released each iteration. `?? 0` supplies a placeholder default
// for the nullable result type so the call result composes into `sink`.
const char* kBoxNullCondMethodCallLoop =
    "module M; "
    "record Rec public x: int; public constructor(x: int) this.x := x; "
    "  end constructor; end Rec; "
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 50 do "
    "    var r := new Rec(j); "
    "    var o: Object := r; "
    "    sink := sink + ((o?.GetHashCode()) ?? 0); " // null-cond direct call
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

}  // namespace

// =====================================================================
// #2060 — a boxed value assigned to an `Object` local read by a direct
// method call (`o.GetHashCode()`) is released at scope end (§8.6 / §28.9);
// no leak. The cardinal repro from the issue.
// =====================================================================

BEZEL_TEST(codegen_box_object_method_call_2060,
           object_method_call_receiver_does_not_leak_spec_8_6_28_9) {
    // spec §8.6 / §28.9, v0.204: a boxed value type is runtime-managed; the box
    // wrapper is freed when the last reference drops. WITHOUT the fix the
    // escape walker treated the method-call receiver as an escape, so no
    // `bz_box_release` was emitted at scope end and every iteration leaked the
    // 24-byte box wrapper (2400 bytes total over 100 iterations) — LSan flips
    // r.ran false. WITH the fix the receiver is a safe read and the box is
    // released exactly once per iteration (no leak, no double-free).
    JitResult r = runAotChecked(kBoxObjectMethodCallLoop);

    BEZEL_EXPECT_TRUE(r.ran);   // LSan-clean (the #2060 leak would flip this)
}

// =====================================================================
// Variant — a boxed primitive (int) with TWO direct method calls per iter.
// =====================================================================

BEZEL_TEST(codegen_box_object_method_call_2060,
           object_method_call_int_box_two_calls_spec_8_6_28_9) {
    JitResult r = runAotChecked(kBoxIntMethodCallLoop);

    BEZEL_EXPECT_TRUE(r.ran);
}

// =====================================================================
// Variant — null-conditional direct method call `o?.GetHashCode()` (§7.5).
// The NullCondCallExpr path must relax the same way.
// =====================================================================

BEZEL_TEST(codegen_box_object_method_call_2060,
           null_cond_method_call_receiver_does_not_leak_spec_8_6_28_9) {
    JitResult r = runAotChecked(kBoxNullCondMethodCallLoop);

    BEZEL_EXPECT_TRUE(r.ran);
}
