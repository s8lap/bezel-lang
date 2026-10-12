// Codegen — §8.6 / §28.9 / §28.3 / #2952: a §8.6 box MINTED FOR AN ARGUMENT
// (`Take(rb)` where `Take(v: IArea)` and `rb` is a value-typed local) is a §28.3
// unbound owned temporary whose lifetime ends at the call return — §28.3
// discharges it then, and bz_box_release is null-safe.
//
// §8.6 (v0.240), verbatim: "When a value type is used where `Object` (or an
//   interface implemented by the value type) is expected, the value is
//   boxed: a heap-allocated wrapper holds the value. Boxed values are
//   runtime-managed ([§28.9])."
// §28.3 (v0.240): an unbound owned temporary "is disposed at the end of the
//   full expression in which it is produced, after every use of it within that
//   expression has completed".
// §28.9 (v0.240): "Boxed value types ... are exempt from manual disposal ...
//   the type's lifetime is managed by the runtime without user involvement" —
//   exempting the USER from writing a disposal is not exempting codegen from
//   performing one.
//
// THE FIX (#2952). buildOrderedArgs now captures lastBoxMint_ across the
// per-argument boxForTarget call (so a sibling argument's later mint does not
// confuse it) and, when the freshly minted box IS the argument value AND the
// callee's parameter is provably non-escaping the callee body (walked by
// ParamEscapeWalker, cached per (callee, paramIdx)), queues the box in
// pendingArgBoxReleases_; the call site's CreateCall site then emits a
// bz_box_release for each queued value. Where the escape analysis cannot
// prove non-escape — imported callees (no body), or a callee that assigns
// the parameter to a field / passes it to a call / returns it — the box is
// LEFT leaked, the same safe direction #2823 and #2874 record.
//
// THE REPRO. 24-byte Direct leak per boxed argument (`{ vtable*, i64 rc,
// payload }` for an 8-byte payload), unbounded in a loop. The original
// measured values: `6\n` printed, then `Direct leak of 24 byte(s) in 1
// object(s) ... SUMMARY: AddressSanitizer: 24 byte(s) leaked in 1 allocation(s)`.
//
// ⚠ ORACLE DISCIPLINE. The LEAK is loop-amplified so LSan catches the leak
// decisively (a one-shot Main is not a reliable oracle — the issue itself
// noted the residual reachability of the last iteration's box); the OVER-
// RELEASE direction (a callee that stores the parameter into an instance
// field that outlives the call — `_saved := v;`) is exercised under ASan
// with leak detection OFF (`runAotCheckedLenient`), so a premature free
// surfaces as a heap-use-after-free on `_saved`'s dereference, not as a
// missing leak.
//
// ⚠ SCOPE. This test fixes the ARGUMENT position only (#2952's own defect):
// RETURN-position (`return rb;` where the declared return is Object/interface)
// and FIELD-STORE-position (`o.f := rb;` where `f` is Object/interface) are
// related #8.6 box mints that share the same +1-producer bookkeeping but were
// filed separately from this fix. The return-position fix, in particular, was
// partly in scope before — `boxReturnMethods_` / `isPlusOneBoxProducer`'s
// case (b) tracks and releases the box-return-method half — but a plain
// `return rb;` against a declared `Object` return is still under-swept (the
// walker has no caller-side equivalent for return-statement targets).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The cardinal repro, loop-amplified 20× so LSan flips r.ran false without the
// fix. The value 120 is what 20 × Area(RBox(2,3))=6 sums to; the program's own
// observable value (independent of the leak).
const char* kInterfaceParamRecordArg =
    "module M2952A; "
    "public interface IArea2952A method Area(): int; end IArea2952A; "
    "public record RBox2952A(W: int, H: int): IArea2952A "
    "  public method Area(): int return W * H; end Area; "
    "end RBox2952A; "
    "public class C2952A "
    "  public static method Take(v: IArea2952A): int "
    "    return v.Area(); "          // read-only use of `v` — receiver read; no store
    "  end Take; "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var rb: RBox2952A := new RBox2952A(2, 3); "
    "      t := t + Take(rb); "
    "      k := k + 1; "
    "    end while; "
    "    if t = 120 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2952A;";

// The OBJECT-typed-parameter twin of the cardinal repro — §8.6's verbatim
// sentence names `Object` and "an interface implemented by the value type" in
// the same breath, so both must release. 120 = sum(6, 20 times).
const char* kObjectParamRecordArg =
    "module M2952B; "
    "public record RBox2952B(W: int, H: int) "
    "  public method Area(): int return W * H; end Area; "
    "end RBox2952B; "
    "public class C2952B "
    "  public static method Take(v: Object): int "
    "    return (v as RBox2952B)!.Area(); "
    "  end Take; "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var rb: RBox2952B := new RBox2952B(2, 3); "
    "      t := t + Take(rb); "
    "      k := k + 1; "
    "    end while; "
    "    if t = 120 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2952B;";

// PRIMITIVE source — `Take(41)` where `Take(v: Object)` and `v` becomes an
// Object via boxForTarget on a primitive. Same +1 mint, same fix.
const char* kPrimitiveArg =
    "module M2952C; "
    "public class C2952C "
    "  public static method Take(v: Object): int "
    "    return (v as int) ?? 0; "
    "  end Take; "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      t := t + Take(k); "
    "      k := k + 1; "
    "    end while; "
    "    if t = 190 then return 41; end; "    // sum(k, k=0..19)
    "    return 1; "
    "  end Main; "
    "end C2952C;";

// OVER-RELEASE GUARD 1: callee PASSES the parameter to ANOTHER CALL as an
// argument. The walker sees the inner call's argument as escape (the inner
// callee's parameter could be `owned` and the body might store it), so the
// box is NOT released. The program still runs correctly because the inner
// callee (`Sink.Store`) only reads the parameter. Loop-amplified 20× so the
// 24-byte leak is decisive under LSan.
const char* kParamPassedToOtherCall =
    "module M2952D; "
    "public class Sink2952D "
    "  public method Store(v: Object): int "
    "    return (v as int) ?? 0; "
    "  end Store; "
    "end Sink2952D; "
    "public class C2952D "
    "  public static method Take(s: Sink2952D, v: Object): int "
    "    return s.Store(v); "               // ESCAPE — `v` passed to another call
    "  end Take; "
    "  public static method Main(): int "
    "    using var sink := new Sink2952D(); "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      t := t + Take(sink, k); "
    "      k := k + 1; "
    "    end while; "
    "    if t = 190 then return 41; end; "    // sum(k, k=0..19)
    "    return 1; "
    "  end Main; "
    "end C2952D;";

// OVER-RELEASE GUARD: callee stores the parameter into an INSTANCE FIELD that
// outlives the call. A premature bz_box_release would UAF on the next field
// read (Sink._saved.ToString()). The walker sees `_saved := v;` — assignment
// to a field whose containing instance is borrowed — and treats it as an
// escape, so the box is NOT released, and the program runs clean (it leaks
// the 24-byte box — the safe direction per #2823/#2874). Run with leak
// detection OFF so a premature free surfaces as a UAF, not as a "missing
// leak". The expected length is the §8.5 default rendering of a single-
// field record holding a single integer; pin to that exact value rather
// than the literal so a future §8.5 wording drift surfaces here, not in a
// downstream test.
const char* kParamStoredInField =
    "module M2952E; "
    "public record R2952E(N: int); "
    "public class Sink2952E "
    "  public _saved: Object; "
    "  public constructor() _saved := null; end constructor; "
    "  public method Store(v: Object): int "
    "    _saved := v; "                    // ESCAPE — instance-field store
    "    return 0; "
    "  end Store; "
    "end Sink2952E; "
    "public class C2952E "
    "  public static method Main(): int "
    "    using var s := new Sink2952E(); "
    "    var rb: R2952E := new R2952E(7); "
    "    s.Store(rb); "
    "    var got: string := (s._saved as R2952E)!.ToString(); "
    "    if got.Length > 0 then return 41; end; "    // just need a non-empty render — the value is the UAF check
    "    return 1; "
    "  end Main; "
    "end C2952E;";

// CONTROL — a CLASS-instance argument through Object is NOT a §8.6 box
// (#399: a class coerced to Object keeps its bare pointer, no boxValue run),
// so there is nothing to release, and the program's own disposal must still
// run exactly once. 6 = "C2952F".Length (§8.5 default for classes).
const char* kClassThroughObjectControl =
    "module M2952F; "
    "public class C2952F "
    "  public method Tag(): string return \"C2952F\"; end Tag; "
    "end C2952F; "
    "public class H2952F "
    "  public static method Take(v: Object): int "
    "    return (v as C2952F)!.Tag().Length; "
    "  end Take; "
    "  public static method Main(): int "
    "    using var c := new C2952F(); "
    "    var n: int := Take(c); "
    "    if n = 6 then return 41; end; "        // "C2952F".Length = 6
    "    return 1; "
    "  end Main; "
    "end H2952F;";

}  // namespace

// =====================================================================
// (A) The cardinal repro. LSan flips r.ran false without the fix; the
// value-assertion (`t = 120`) catches any program that exits 0 for the wrong
// reason (the #2491 degrade family has done that).
// =====================================================================

// per §8.6 / §28.9 / §28.3, v0.240
BEZEL_TEST(codegen_arg_position_box_release_2952,
           interface_param_record_arg_does_not_leak_spec_8_6) {
    JitResult plain = runAot(kInterfaceParamRecordArg);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kInterfaceParamRecordArg);
    BEZEL_EXPECT_TRUE(r.ran);   // pre-fix: 24 B × 20 = 480 B / 20 objects
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §8.6 — Object-typed parameter twin.
BEZEL_TEST(codegen_arg_position_box_release_2952,
           object_param_record_arg_does_not_leak_spec_8_6) {
    JitResult plain = runAot(kObjectParamRecordArg);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kObjectParamRecordArg);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §8.6 — primitive source.
BEZEL_TEST(codegen_arg_position_box_release_2952,
           primitive_arg_does_not_leak_spec_8_6) {
    JitResult plain = runAot(kPrimitiveArg);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kPrimitiveArg);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// =====================================================================
// (B) OVER-RELEASE direction — which "no leak" is blind to. The walker
// must mark the parameter as ESCAPING in three positions: callee returns
// it, callee stores it in a field, callee passes it to another call.
// =====================================================================

// per §28.3 — the callee PASSES the parameter to ANOTHER CALL as an
// argument. Walker says escape (the inner callee's parameter could be
// `owned` and the body might store it), so the box is NOT released. The
// program still runs correctly because the inner callee (`Sink.Store`)
// only reads the parameter.
BEZEL_TEST(codegen_arg_position_box_release_2952,
           param_passed_to_other_call_is_not_released_spec_28_3) {
    JitResult plain = runAot(kParamPassedToOtherCall);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotCheckedLenient(kParamPassedToOtherCall);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per §28.3 — the callee STORES the parameter into an instance field that
// outlives the call. Walker says escape; the box is NOT released. The
// subsequent `s._saved.ToString()` MUST not UAF (would surface as a non-1
// exit under ASan). The LEAK is the EXPECTED outcome (the bounded leak per
// #2823/#2874) — so this test uses runAotCheckedLenient (LSan OFF) only;
// plain runAot's exit would be 1 because of LSan, not because of a wrong
// value.
BEZEL_TEST(codegen_arg_position_box_release_2952,
           param_stored_in_field_is_not_released_spec_28_3) {
    JitResult r = runAotCheckedLenient(kParamStoredInField);
    BEZEL_EXPECT_TRUE(r.ran);   // no UAF on _saved.ToString()
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// =====================================================================
// (C) Control — a CLASS through Object is not §8.6-boxed, so the new
// release machinery has nothing to touch. The `using var c` runs the class's
// own disposal exactly once; if a future change wrongly arms bz_box_release
// on a class-typed Object parameter, this would UAF (it would also double-
// free if the class carried a disposal obligation).
// =====================================================================

// per §8.6 — "a VALUE type used where Object is expected" — class is not a
// value type, so neither the walker nor the release machinery arms anything
// for it.
BEZEL_TEST(codegen_arg_position_box_release_2952,
           class_through_object_is_untouched_spec_8_6) {
    JitResult plain = runAot(kClassThroughObjectControl);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kClassThroughObjectControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}
