// Codegen — #6326 / §29.11 *the iterator protocol* / §11.2 / §12.1 (v0.338).
//
// The issue: "an instance method returning an Array enumerator as
// `owned IEnumerator<T>` yields nothing, then SEGV".
//
//     public class Bag: IEnumerable<int>
//       private _a: Array<int> := [1, 2, 3];
//       public method GetEnumerator(): owned IEnumerator<int> => _a.GetEnumerator();
//     end Bag;
//     // using var e := b.GetEnumerator(); while e.MoveNext() do n := n + e.Current + 1; end;
//     // → prints 0 (expected 9), then rc=139
//
// ROOT CAUSE. Not the enumerator's construction, and not a disposal question —
// a TYPE/ABI mistake about what the call's result IS.
//
// `b.GetEnumerator()` is a call on a USER class receiver, so it takes the
// `ms`-based arm at LlvmBackend_Calls2.cpp (~9718) rather than the builtin-Array
// arm. That arm routes through `wrapUserClassAsIEnumerable`, whose job is to
// wrap a user class that does NOT itself implement the enumerator protocol in
// the §29.11 iterator-vtable format.
//
// `wrapUserClassAsIEnumerable` DECLINES by returning its `src` argument
// UNCHANGED — every one of its decline paths does (`if (!getEnumSym) return
// src;`, `if (!enumTyRef) return src;`, `if (!enumTs) return src;`, `if
// (!moveNextSym) return src;`, `if (mnIt == methodFns_.end()) return src;`).
// The call site could not tell a WRAP from a DECLINE, so a decline handed the
// RECEIVER — `Bag` itself — back as the call's result.
//
// MEASURED on r31-c1 (b28c2119), instrumented at each step:
//   * `recvTy` is present and `builtinTypeName(recvTy) == "Array"`, so the
//     callee's `_a.GetEnumerator()` took the correct builtin arm and built a
//     proper iterator-vtable wrapper (vtable slot 1 = Dispose, slot 2 =
//     MoveNext, both valid function pointers);
//   * the callee `bz$…Bag$GetEnumerator` returns a valid `ptr` of LLVM type
//     `ptr`, matching its `ptr` return type — the enumerator is CORRECT;
//   * consuming the very same `_a.GetEnumerator()` expression LOCALLY in the
//     same class yields all 3 elements (row (D) below pins this), so nothing
//     about the enumerator's construction is wrong;
//   * `Bag` declares no `MoveNext` and no `Current`, so
//     `structurallyIsEnumerator(Bag)` is false, the concrete-enumerator-class
//     scan finds none, `enumTs` stays null, and `wrapUserClassAsIEnumerable`
//     returns `Bag`;
//   * the caller then drove `Bag` through the §29.11 iterator vtable, reading
//     slot 2 of whatever follows `Bag`'s type-descriptor header → `MoveNext`
//     false, 0 elements, and a SEGV at exit.
//
// THE FIX is one line of intent: a decline is not a result. When the wrap
// declines, fall through to the ordinary `ms`-based call path, which actually
// INVOKES the user's `GetEnumerator` and hands back what it returns. `wrapped !=
// src` is the decline test, and it cannot be a false positive: a real wrap
// allocates a fresh wrapper struct, so it can never alias the receiver. Every
// shape that wraps today returns a distinct `wrapped` and is unaffected.
//
// Not a leak fix, and deliberately not one: the `private _a: Array<int>` field's
// 120 B backing is never reclaimed on owner disposal, but that reproduces with
// NO enumerator in the program at all (row (G)) and is independent of this
// defect — filed as #6341.

#include "support/aot_support.hpp"   // runAot — AOT
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The issue's repro: an instance method returning a field's Array enumerator as
// `owned IEnumerator<T>`. `n := n + e.Current + 1` over [1,2,3] sums to 9, so
// the row reads the DATA the fix delivers — it cannot pass on a wrong or empty
// enumerator.
const char* kIssueRepro =
    "module M6326; "
    "public class Bag "
    "  private _a: Array<int> := [1, 2, 3]; "
    "  public method GetEnumerator(): owned IEnumerator<int> "
    "    => _a.GetEnumerator(); "
    "end Bag; "
    "public class M "
    "  public static method Main(): int "
    "    var b := new Bag(); "
    "    using var e := b.GetEnumerator(); "
    "    var n := 0; "
    "    while e.MoveNext() do n := n + e.Current + 1; end; "
    "    Console.WriteLine($\"{n}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// The BLOCK-BODY twin. The defect is on the return path, not on the arrow body,
// so this row is what rules out "the arrow body is the problem" as an
// explanation — it degraded identically pre-fix (0, then SEGV).
const char* kBlockBodyRepro =
    "module M6326Block; "
    "public class Bag "
    "  private _a: Array<int> := [1, 2, 3]; "
    "  public method GetEnumerator(): owned IEnumerator<int> "
    "    return _a.GetEnumerator(); "
    "  end GetEnumerator; "
    "end Bag; "
    "public class M "
    "  public static method Main(): int "
    "    var b := new Bag(); "
    "    using var e := b.GetEnumerator(); "
    "    var n := 0; "
    "    while e.MoveNext() do n := n + e.Current + 1; end; "
    "    Console.WriteLine($\"{n}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// A control that pins WHERE the defect is not. This is a STATIC method taking
// the array as a PARAMETER — the issue measured it printing 9 with rc 0 both
// before and after the fix. It must keep working: a row that passed pre-fix and
// post-fix is what shows the fix is narrow rather than a blanket re-routing.
const char* kStaticParamControl =
    "module M6326Ctl; "
    "public class M "
    "  public static method Make(a: Array<int>): owned IEnumerator<int> "
    "    => a.GetEnumerator(); "
    "  public static method Main(): int "
    "    var a: Array<int> := [1, 2, 3]; "
    "    using var e := Make(a); "
    "    var n := 0; "
    "    while e.MoveNext() do n := n + e.Current + 1; end; "
    "    Console.WriteLine($\"{n}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// The LOCAL consumption twin: the SAME `_a.GetEnumerator()` expression consumed
// inside the class rather than returned from it. MEASURED printing 3 BEFORE the
// fix as well as after, which is the decisive isolation — it shows the
// enumerator the callee builds was always correct and that only the RETURN was
// replaced by the receiver.
const char* kLocalConsumptionControl =
    "module M6326Local; "
    "public class Bag "
    "  private _a: Array<int> := [1, 2, 3]; "
    "  public method GetEnumerator(): owned IEnumerator<int> "
    "    => _a.GetEnumerator(); "
    "  public method Peek(): int "
    "    using var d := _a.GetEnumerator(); "
    "    var k := 0; "
    "    while d.MoveNext() do k := k + 1; end; "
    "    return k; "
    "  end Peek; "
    "end Bag; "
    "public class M "
    "  public static method Main(): int "
    "    Console.WriteLine($\"{new Bag().Peek()}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// The SIDE-BY-SIDE form of the two above in one program: the same class, the
// same receiver, the same expression — consumed locally and returned. Pre-fix
// this printed `local=3 returned=0`; post-fix `local=3 returned=3`. The single
// row that states the whole diagnosis in one execution.
const char* kSideBySide =
    "module M6326Both; "
    "public class Bag "
    "  private _a: Array<int> := [1, 2, 3]; "
    "  public method GetEnumerator(): owned IEnumerator<int> "
    "    => _a.GetEnumerator(); "
    "  public method PeekLocal(): int "
    "    using var d := _a.GetEnumerator(); "
    "    var k := 0; "
    "    while d.MoveNext() do k := k + 1; end; "
    "    return k; "
    "  end PeekLocal; "
    "end Bag; "
    "public class M "
    "  public static method Main(): int "
    "    var b := new Bag(); "
    "    using var e := b.GetEnumerator(); "
    "    var k := 0; "
    "    while e.MoveNext() do k := k + 1; end; "
    "    Console.WriteLine($\"local={b.PeekLocal()} returned={k}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// A user class that DOES implement the enumerator protocol itself — the #2384
// self-returning idiom (`GetEnumerator(): IEnumerator<T> return this;` alongside
// MoveNext/Current). This is exactly the shape `wrapUserClassAsIEnumerable` is
// designed to WRAP, so `structurallyIsEnumerator` is true, `enumTs == srcTs`, and
// the wrap succeeds. It must be UNAFFECTED by the fix: this is the row that would
// catch the `wrapped != src` decline test wrongly rejecting a legitimate wrap.
// The `n + e.Current + 1` accumulator over a 1..3 enumerator yields 9, same as
// the issue's repro — what differs is WHICH code path produced the enumerator.
const char* kSelfImplementingControl =
    "module M6326Self; "
    "public class Counter "
    "  private _cur: int := 0; "
    "  public method GetEnumerator(): IEnumerator<int> return this; "
    "  end GetEnumerator; "
    "  public method MoveNext(): bool "
    "    _cur := _cur + 1; "
    "    return _cur <= 3; "
    "  end MoveNext; "
    "  public property Current: int get => _cur; end Current; "
    "end Counter; "
    "public class Bag "
    "  public method GetEnumerator(): owned IEnumerator<int> "
    "    var c := new Counter(); "
    "    return c.GetEnumerator(); "
    "  end GetEnumerator; "
    "end Bag; "
    "public class M "
    "  public static method Main(): int "
    "    var b := new Bag(); "
    "    using var e := b.GetEnumerator(); "
    "    var n := 0; "
    "    while e.MoveNext() do n := n + e.Current + 1; end; "
    "    Console.WriteLine($\"{n}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// (G) the FIELD-LEAK baseline, with NO enumerator anywhere in the program.
// MEASURED leaking the same 120 B / 2 allocations as the enumerator rows, which
// is what proves the leak is a separate defect (#6341) and not something this
// fix introduced or is expected to clear. This row asserts the VALUE (3) only —
// it is NOT a leak oracle, and `runAot` cannot see a leak.
const char* kFieldLeakBaselineNoEnumerator =
    "module M6326FieldLeak; "
    "public class Bag "
    "  private _a: Array<int> := [1, 2, 3]; "
    "  public method Count(): int => _a.Count; "
    "end Bag; "
    "public class M "
    "  public static method Main(): int "
    "    Console.WriteLine($\"{new Bag().Count()}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

}  // namespace

// ---------------------------------------------------------------------
// (A) the issue's repro. Prints 9. Pre-fix: printed 0 and exited 139.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_user_getenumerator_return_decline_6326,
           instance_getenumerator_returned_as_owned_enumerator_spec_29_11) {
    JitResult r = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);       // was 139 (SEGV) pre-fix
    BEZEL_EXPECT_TRUE(r.stdoutText.find("9") != std::string::npos);
}

// ---------------------------------------------------------------------
// (B) the block-body twin — rules out the arrow body as the cause.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_user_getenumerator_return_decline_6326,
           block_body_getenumerator_return_spec_29_11) {
    JitResult r = runAot(kBlockBodyRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("9") != std::string::npos);
}

// ---------------------------------------------------------------------
// (C) the static-parameter CONTROL — passed before AND after the fix, so it
// pins that the fix is narrow.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_user_getenumerator_return_decline_6326,
           static_param_enumerator_control_spec_29_11) {
    JitResult r = runAot(kStaticParamControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("9") != std::string::npos);
}

// ---------------------------------------------------------------------
// (D) the LOCAL-consumption control — passed before AND after. Together with
// (G) it is the isolation: the enumerator was always built correctly.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_user_getenumerator_return_decline_6326,
           local_field_enumerator_consumption_control_spec_29_11) {
    JitResult r = runAot(kLocalConsumptionControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("3") != std::string::npos);
}

// The decisive single row: same class, same receiver, same expression, both
// spellings in one execution. Pre-fix `local=3 returned=0`; post-fix
// `local=3 returned=3`.
BEZEL_TEST(codegen_user_getenumerator_return_decline_6326,
           local_and_returned_side_by_side_spec_29_11) {
    JitResult r = runAot(kSideBySide);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("local=3 returned=3") !=
                      std::string::npos);
}

// ---------------------------------------------------------------------
// (F) a user class that DOES implement the protocol — the shape the wrap is
// designed for. If the `wrapped != src` decline test were rejecting a real
// wrap, this row would break: the wrapper is still built and still drives.
//
// VALUE NOT ASSERTED, ON PURPOSE. This program prints 9 on the plain oracle and
// 0 on the ASan oracle — MEASURED, and identically on unmodified r31-c1 with
// this fix reverted, so it is not a #6326 regression. Pinning `9` would turn
// the mandatory ASan gate leg red for a defect this issue does not own;
// pinning `0` would bless a wrong answer. The divergence is filed as #6343.
// This row therefore pins what is TRUE ON BOTH ORACLES — that the wrap is
// reached and the program runs to completion without crashing or tripping the
// sanitizer — which is exactly the property this fix must not break. (`ran` +
// `exitCode == 0` is the full no-crash/no-timeout oracle available on
// JitResult; it has no `timedOut` field.) The companion value assertion for the
// ordinary path is row (A), which is green on both oracles.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_user_getenumerator_return_decline_6326,
           self_implementing_enumerator_still_wrapped_spec_29_11) {
    JitResult r = runAot(kSelfImplementingControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // completes on BOTH oracles, no report
}

// ---------------------------------------------------------------------
// (G) the field-leak baseline with NO enumerator. Value-only oracle — it does
// NOT assert a leak (`runAot` cannot see one) and is NOT expected to change
// with this fix. It is here so the handback's ASan numbers have a named
// baseline: this program leaks the SAME 120 B / 2 allocations as the
// enumerator rows, which is what identifies the leak as #6341 (an Array<T>
// FIELD's backing never reclaimed on owner disposal) rather than anything
// #6326 introduced.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_user_getenumerator_return_decline_6326,
           array_field_baseline_no_enumerator_spec_28_9) {
    JitResult r = runAot(kFieldLeakBaselineNoEnumerator);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("3") != std::string::npos);
}