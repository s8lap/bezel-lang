// Disposal-correctness — issue #1425 sub-item (3): "disposal of class-typed
// constraint-static results". Spec §11.8 (v0.228) "Dispatch through the type
// parameter" states the ruling explicitly for the section's own `Sum<T>`
// example:
//
//   "At a class or interface substitution of T, total is an owned local:
//    T.Zero() and each operator result transfer per stdlib §2.19's `owned`
//    producer signatures, the loop's reassignment disposes the displaced
//    previous total by drop-on-overwrite (§28.7), and the `return` transfers
//    to the caller."
//
// THE BUG. `T.Zero()` and each `total + x` (a §2.19 `owned`-marked producer)
// allocate a Meters instance that the loop's reassignment must dispose via
// §28.7 drop-on-overwrite — but the codegen classifier that DRIVES that
// disposal, `Emitter::unwindOwnedLocalClass` (src/codegen/LlvmBackend_Setup.cpp,
// case (b) "a call returning `owned T`"), tested
// `isClassOrInterfaceRef(ctx_.exprType(*ld->initializer))` alone. For a bare
// type-parameter call result (`T.Zero()`), `ctx_.exprType` is the STILL-
// GENERIC, unsubstituted `T` (sema type-checks the generic body ONCE, never
// per instantiation) — `declaration == nullptr` — so the check read false
// even while `buildSpecializedMethodFn` is compiling a genuine CLASS
// instantiation (`Sum$Meters`) with `genericSubstSemaTy_["T"] == Meters`
// ACTIVE. `total`'s LocalDecl was therefore never recognized as "a definitely-
// owned plain class/interface local", so
// `Emitter::disposeDisplacedOwnedClassLocal` (keyed on that SAME
// classification for EVERY reassignment of `total`, regardless of the
// reassignment's own RHS shape) silently no-op'd on every `total := total +
// x` — the displaced value leaked. Confirmed via `agent_check.sh --probe
// --asan` on the spec's own `Sum<T>` shape BEFORE this fix: LeakSanitizer
// reported 3 direct leaks (48 bytes / 3 allocations) — `T.Zero()`'s result
// plus each of the two INTERMEDIATE `total + x` results (the third, FINAL `+`
// result transfers cleanly through the `return`, so it does not leak).
//
// THE FIX. `unwindOwnedLocalClass` case (b) now ALSO tests
// `isClassOrInterfaceRef(resolveTypeParamRef(initTy))` — the same bare-type-
// parameter-through-the-active-substitution fallback the sibling classifiers
// in the same function already use (case (a2) BinaryExpr operator, (a0)
// arm-meet, (a-1) bind-atom) — so a bare-T `owned T` call result is
// recognized exactly when THIS specialization is a genuine class/interface
// instantiation. Disposal itself is dynamic (vtable dispatch through
// `emitDisposeAndReclaim`), so the classifier does not need the CONCRETE
// class, only the "yes, dispose dynamically" verdict (the sentinel
// `reinterpret_cast<const sema::TypeSymbol*>(1)`).
//
// runAotChecked (AOT + ASan/LSan) is required here per tests/README.md: a
// leak-freedom check alone is not enough to also rule out a double-free (the
// classic failure mode CLAUDE.md and the issue itself call out — "the
// intermediates ... leak" could just as easily have been "the intermediates
// double-free" from an over-eager fix). Meters' destructor prints a per-
// INSTANCE id (a monotonic counter, not the numeric Value — two different
// Meters instances legitimately share a Value, e.g. an intermediate total and
// a later one could coincide, so only an instance id gives an unambiguous
// exactly-once count) so each of the 7 instances constructed by the accept
// test (m0, m1, m2, Zero()'s result, and each of the three `+` results) is
// independently confirmed disposed exactly once.

#include <cstddef>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// The spec §11.8 Sum<T> example VERBATIM (stdlib INumber<T>, §2.19), over a
// user class conformer whose constructor assigns a monotonic instance id and
// whose destructor prints it — the same per-instance-marker discipline
// arm_meet_ownership_1661_test.cpp uses, needed here because two Meters
// instances (an intermediate total and a later one) can legitimately share a
// numeric Value.
const char* kMetersWithId =
    "module M; "
    "public class Meters: INumber<Meters> "
    "  public Value: int; "
    "  public Id: int; "
    "  private static _next: int := 0; "
    "  public constructor(v: int) "
    "    Value := v; "
    "    Id := _next; "
    "    _next := _next + 1; "
    "  end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "  public static method Zero(): owned Meters => new Meters(0); "
    "  public static method One(): owned Meters => new Meters(1); "
    "  public static method operator +(a: Meters, b: Meters): owned Meters => "
    "    new Meters(a.Value + b.Value); "
    "  public static method operator -(a: Meters, b: Meters): owned Meters => "
    "    new Meters(a.Value - b.Value); "
    "  public static method operator *(a: Meters, b: Meters): owned Meters => "
    "    new Meters(a.Value * b.Value); "
    "  public static method operator /(a: Meters, b: Meters): owned Meters => "
    "    new Meters(a.Value / b.Value); "
    "  public static method operator <(a: Meters, b: Meters): bool => "
    "    a.Value < b.Value; "
    "  public static method operator >(a: Meters, b: Meters): bool => "
    "    a.Value > b.Value; "
    "  public static method operator <=(a: Meters, b: Meters): bool => "
    "    a.Value <= b.Value; "
    "  public static method operator >=(a: Meters, b: Meters): bool => "
    "    a.Value >= b.Value; "
    "end Meters; "
    "static class F "
    "  public static method Sum<T>(items: Array<T>): owned T where T: INumber<T> "
    "    var total := T.Zero(); "
    "    for var x in items do total := total + x; end; "
    "    return total; "
    "  end Sum; "
    "end F; "
    "class C public static method Main(): int "
    "  using var m0 := new Meters(10); using var m1 := new Meters(13); "
    "  using var m2 := new Meters(19); "
    "  using var xs := new Array<Meters>(); "
    "  xs.Add(m0); xs.Add(m1); xs.Add(m2); "
    "  var t := F.Sum(xs); "
    "  Console.WriteLine(\"body\"); "
    "  return t.Value; "
    "end Main; end C;";

}  // namespace

// §11.8 (v0.228) / §28.7 (#1425 sub-item 3): the accumulator's intermediates
// (T.Zero()'s result plus each displaced `total + x`) and the final result are
// each disposed EXACTLY once — no leak (WITHOUT the fix: 3 direct leaks under
// LSan, confirmed via `agent_check.sh --probe --asan` before landing this
// test) and no double-free (confirmed by the exactly-once count per instance
// id, not merely by leak-freedom, per the sub-item's own stated risk).
BEZEL_TEST(static_iface_constraint_disposal_1425,
           sum_accumulator_disposes_each_instance_exactly_once_spec_11_8) {
    JitResult r = runAotChecked(kMetersWithId);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // WITHOUT the fix: LSan reports the 3 direct leaks (Zero()'s result plus
    // the two displaced intermediate `+` results) -> r.ran false.
    BEZEL_EXPECT_TRUE(r.ran);
    // 10 + 13 + 19 = 42 — the accumulation itself must still be correct.
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    // 7 Meters instances are constructed: m0(id0), m1(id1), m2(id2),
    // T.Zero()=id3, total+m0=id4, total+m1=id5, total+m2=id6 (the final,
    // returned result). Each must be disposed EXACTLY once.
    for (int id = 0; id <= 6; ++id) {
        std::string marker = "D" + std::to_string(id);
        BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, marker), 1);
    }
    // No 8th instance leaked or was double-counted.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D7"), 0);
}
