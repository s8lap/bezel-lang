// Codegen — §8.6 / §28.9 (#2960): plain `:=` into an Object/interface
// local must release a displaced §8.6 box and keep the replacement box alive
// until its next overwrite or scope exit.
//
// Spec (v0.240):
//   §8.6 "Boxing" — "When a value type is used where `Object` (or an
//   interface implemented by the value type) is expected, the value is
//   **boxed**: a heap-allocated wrapper holds the value. Boxed values are
//   runtime-managed ([§28.9])."
//   §28.9 "Runtime-managed types" — boxed value types are exempt from manual
//   disposal; their lifetime is managed without user involvement.
//
// The safety boundary is box-vs-class placement, not the shared pointer shape:
// a class coerced to Object/interface stays a bare class pointer and must never
// reach bz_box_release. The mixed programs below exercise both transition
// directions (class -> box and box -> class), including an interface slot. The
// escaping-local control proves that passing the old box to a callee that stores
// it suppresses local box management rather than prematurely freeing the alias.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 6 absorbing rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it —
// the definition scripts/check_toolchain_skip_surface.py uses for an
// ABSORBING site. It was also a silent vacuous pass: on a box whose clang
// cannot link the ASan binary, every one of those rows returned WITHOUT
// asserting, and the suite reported a clean PASS having compiled and run
// nothing.
//
// Each guard becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is still recognised as an environment condition rather than a
// product failure, but it now MARKS THE ROW FAILED instead of hiding it.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

const char* kObjectBoxReassignLoop =
    "module M2960A; "
    "class C2960A "
    "  public static method Main(): int "
    "    var i := 0; "
    "    var p: Object := 1; "
    "    while i < 100 do "
    "      p := i + 5; "
    "      i := i + 1; "
    "    end; "
    "    var n: int? := p as int; "
    "    if n <> null and n! = 104 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2960A;";

const char* kObjectNonBoxThenBox =
    "module M2960B; "
    "class C2960B "
    "  public static method Main(): int "
    "    var p: Object := \"seed\"; "
    "    p := 5; "
    "    if p is int then return 42; end; "
    "    return 1; "
    "  end Main; "
    "end C2960B;";

const char* kObjectClassBoxTransitions =
    "module M2960C; "
    "class K2960C "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end K2960C; "
    "class C2960C "
    "  public static method Main(): int "
    "    var i := 0; var seen := 0; "
    "    while i < 50 do "
    "      using var c := new K2960C(i); "
    "      var p: Object := c; "
    "      p := i; "
    "      if p is int then seen := seen + 1; end; "
    "      p := c; "
    "      if p is K2960C then seen := seen + 1; end; "
    "      seen := seen + c.V - c.V; "
    "      i := i + 1; "
    "    end; "
    "    if seen = 100 then return 43; end; "
    "    return 1; "
    "  end Main; "
    "end C2960C;";

const char* kInterfaceClassBoxTransitions =
    "module M2960D; "
    "interface IArea2960D method Area(): int; end IArea2960D; "
    "record RBox2960D(W: int, H: int) : IArea2960D "
    "  public method Area(): int => W * H; "
    "end RBox2960D; "
    "class KArea2960D : IArea2960D "
    "  public method Area(): int => 7; "
    "end KArea2960D; "
    "class C2960D "
    "  public static method Main(): int "
    "    var i := 0; var sum := 0; "
    "    while i < 40 do "
    "      using var c := new KArea2960D(); "
    "      var p: IArea2960D := c; "
    "      p := new RBox2960D(2, 3); "
    "      sum := sum + p.Area(); "
    "      p := c; "
    "      sum := sum + p.Area(); "
    "      i := i + 1; "
    "    end; "
    "    if sum = 520 then return 44; end; "
    "    return 1; "
    "  end Main; "
    "end C2960D;";

const char* kThrowBeforeReplacementStore =
    "module M2960E; "
    "class AppError2960E: Exception end AppError2960E; "
    "class C2960E "
    "  public static method Fail(i: int): int "
    "    if i >= 0 then throw new AppError2960E(); end; "
    "    return i; "
    "  end Fail; "
    "  public static method Main(): int "
    "    var i := 0; var caught := 0; "
    "    while i < 30 do "
    "      try "
    "        var p: Object := i; "
    "        p := Fail(i); "
    "      catch AppError2960E => caught := caught + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    if caught = 30 then return 45; end; "
    "    return 1; "
    "  end Main; "
    "end C2960E;";

const char* kEscapingBoxStoredByCallee =
    "module M2960F; "
    "class Sink2960F "
    "  public O: Object := \"seed\"; "
    "  public method Save(o: Object) O := o; end Save; "
    "  public method Read(): int "
    "    var n: int? := O as int; "
    "    return n ?? 0; "
    "  end Read; "
    "end Sink2960F; "
    "class C2960F "
    "  public static method Main(): int "
    "    using var sink := new Sink2960F(); "
    "    var p: Object := 7; "
    "    sink.Save(p); "
    "    p := 9; "
    "    return sink.Read(); "
    "  end Main; "
    "end C2960F;";

}  // namespace

// per §8.6 / §28.9, v0.240: every displaced loop box and the final box are
// runtime-managed. Pre-fix LSan reports one 24-byte allocation per overwrite.
BEZEL_TEST(codegen_box_local_reassign_2960,
           object_box_overwrite_loop_releases_displaced_and_final_spec_8_6_28_9) {
    JitResult plain = runAot(kObjectBoxReassignLoop);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult checked = runAotChecked(kObjectBoxReassignLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(checked));
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 41);
}

// per §8.6 / §28.9, v0.240: a non-box initializer must not prevent the later
// fresh box from acquiring a scope-end release.
BEZEL_TEST(codegen_box_local_reassign_2960,
           object_nonbox_then_box_releases_final_spec_8_6_28_9) {
    JitResult checked = runAotChecked(kObjectNonBoxThenBox);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(checked));
    BEZEL_EXPECT_TRUE(checked.ran);  // pre-fix: 24 B in one allocation
    BEZEL_EXPECT_EQ(checked.exitCode, 42);
}

// per §8.6 / §28.9, v0.240: the release discriminator follows the value stored
// in the slot. It releases the box on box -> class, never the class pointer on
// class -> box or at scope exit.
BEZEL_TEST(codegen_box_local_reassign_2960,
           object_class_box_transitions_preserve_class_spec_8_6_28_9) {
    JitResult checked = runAotChecked(kObjectClassBoxTransitions);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(checked));
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 43);
}

// per §8.6 / §28.9, v0.240: same box-vs-class discriminator for an
// interface-typed slot, including interface dispatch before both overwrites.
BEZEL_TEST(codegen_box_local_reassign_2960,
           interface_class_box_transitions_preserve_class_spec_8_6_28_9) {
    JitResult checked = runAotChecked(kInterfaceClassBoxTransitions);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(checked));
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 44);
}

// per §28.3 / §28.9, v0.240: RHS evaluation precedes displaced-value release.
// If the RHS throws, unwind cleanup releases the still-current old box.
BEZEL_TEST(codegen_box_local_reassign_2960,
           throwing_rhs_releases_current_box_on_unwind_spec_28_3_28_9) {
    JitResult checked = runAotChecked(kThrowBeforeReplacementStore);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(checked));
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 45);
}

// per §28.3 / §28.9, v0.240: a call argument is an escape. The callee stores
// the old box, so local reassignment must not release it. Leak detection is off
// deliberately: the conservative escaping-box path is expected to retain.
BEZEL_TEST(codegen_box_local_reassign_2960,
           callee_stored_escape_survives_local_reassignment_spec_28_3_28_9) {
    JitResult checked = runAotCheckedLenient(kEscapingBoxStoredByCallee);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(checked));
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 7);
}
