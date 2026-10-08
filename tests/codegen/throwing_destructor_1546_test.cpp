// Codegen — §28.4 throwing-destructor defined behavior (#1546, v0.180
// conformance batch).
//
// docs/Bezel_Language_Spec.md §28.4 (v0.180):
//   "Destructors should not throw; defined behavior when one does ... A
//   destructor body (and hence the disposal it customizes) is expected not
//   to throw. When one does, behavior is defined, not undefined:
//    - The exception propagates from the disposal point ...
//    - Before it propagates, the remaining steps of the generated Dispose()
//      still run: the remaining owned-field disposals (reverse declaration
//      order) and the base-class chain complete, and physical reclamation
//      still occurs — a cleanup fault never leaks the object."
//
// Fix: `Emitter::emitDisposerCore` (src/codegen/LlvmBackend_Disposal.cpp)
// previously let a `throw` directly inside a `destructor` body branch
// straight to the function's unwind/propagate block, which for the
// disposer-core function is an immediate `ret void` — skipping the owned-
// field cascade and base-class chain entirely (a leak of every field
// declared after the point of the throw, plus of the base subobject's own
// cleanup). The fix installs a LOCAL unwind target for the body so a throw
// (or a propagating call) inside it joins the same post-body cleanup path
// used for normal completion, before the exception is left for the caller.
//
// These tests observe the fix via static counters bumped by each
// destructor, mirroring the destructor-count convention of
// exception_disposal_test.cpp. Each program returns the SAME weighted
// counter sum from the `try`'s normal exit and from its `catch` arm, so
// the assertion is about the counters alone and is independent of where
// the fault is observed.
//
// That independence was originally load-bearing: when #1546 landed, a
// throwing destructor's exception was not caught by the enclosing method's
// own pending checks at the disposal call site (a separate, broader gap —
// the exception actually PROPAGATING from the disposal point to a caller's
// `try`/`catch`), so the counters had to be readable from whichever site
// first happened to observe the pending exception. That gap is now CLOSED
// (§28.4, issues #1878/#1882): `o.Dispose()` propagates from the disposal
// point and the `catch` arm below is what runs. The rows are unchanged —
// both exits still return the same value — and they keep proving what
// #1546 is about: that every remaining generated `Dispose()` step ran,
// since the counters are bumped strictly during `Dispose()`, before any
// of the dispatching happens.

#include "support/codegen_support.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §28.4 — a throwing destructor still disposes the remaining owned field(s)
// declared after it in the generated Dispose() sequence.
const char* kOwnedFieldStillDisposedWhenDestructorThrows =
    "module M; "
    "class AppError: Exception end AppError; "
    "class Field "
    "  public static _n: int := 0; "
    "  destructor Field._n := Field._n + 1; end destructor; "
    "end Field; "
    "class Owner "
    "  private owned _f: Field; "
    "  public constructor(owned f: Field) _f := f; end constructor; "
    "  destructor throw new AppError(); end destructor; "
    "end Owner; "
    "class Program "
    "  public static method Main(): int "
    "    var f := new Field(); "
    "    var o := new Owner(move f); "
    "    try "
    "      o.Dispose(); "
    "      return Field._n; "
    "    catch "
    "      AppError => return Field._n; "
    "    end; "
    "  end Main; "
    "end Program;";

BEZEL_TEST(codegen_throwing_destructor_1546,
           owned_field_still_disposed_when_destructor_throws) {
    BEZEL_EXPECT_EQ(run(kOwnedFieldStillDisposedWhenDestructorThrows), 1);
}

// §28.4 — ALL remaining steps run: the destructor body's own (pre-throw)
// statements, the owned-field cascade, AND the base-class chain. Encoded as
// a weighted sum (100*bodyRan + 10*fieldRan + baseRan) so a single exit
// code proves all three counters are exactly 1.
const char* kBodyFieldAndBaseChainAllRunWhenDestructorThrows =
    "module M; "
    "class AppError: Exception end AppError; "
    "class Base "
    "  public static _baseRan: int := 0; "
    "  destructor Base._baseRan := Base._baseRan + 1; end destructor; "
    "end Base; "
    "class Field "
    "  public static _fieldRan: int := 0; "
    "  destructor Field._fieldRan := Field._fieldRan + 1; end destructor; "
    "end Field; "
    "class Owner: Base "
    "  public static _bodyRan: int := 0; "
    "  private owned _f: Field; "
    "  public constructor(owned f: Field) _f := f; end constructor; "
    "  destructor "
    "    Owner._bodyRan := Owner._bodyRan + 1; "
    "    throw new AppError(); "
    "  end destructor; "
    "end Owner; "
    "class Program "
    "  public static method Main(): int "
    "    var f := new Field(); "
    "    var o := new Owner(move f); "
    "    try "
    "      o.Dispose(); "
    "      return 100 * Owner._bodyRan + 10 * Field._fieldRan + Base._baseRan; "
    "    catch "
    "      AppError => return 100 * Owner._bodyRan + 10 * Field._fieldRan + "
    "                          Base._baseRan; "
    "    end; "
    "  end Main; "
    "end Program;";

BEZEL_TEST(codegen_throwing_destructor_1546,
           body_field_and_base_chain_all_run_when_destructor_throws) {
    BEZEL_EXPECT_EQ(run(kBodyFieldAndBaseChainAllRunWhenDestructorThrows),
                    111);
}

// §28.4 — the owned-field cascade that still runs after a throwing
// destructor preserves the normal reverse-declaration-order rule (§28.5
// item 1): `_f2` (declared second) is disposed BEFORE `_f1` (declared
// first). Each field's destructor stamps a shared step counter, so
// `_f1Step`/`_f2Step` record which one ran first.
const char* kFieldDisposalOrderPreservedWhenDestructorThrows =
    "module M; "
    "class AppError: Exception end AppError; "
    "class F1 "
    "  public static _step: int := 0; "
    "  public static _f1Step: int := 0; "
    "  public static _f2Step: int := 0; "
    "  destructor "
    "    F1._step := F1._step + 1; "
    "    F1._f1Step := F1._step; "
    "  end destructor; "
    "end F1; "
    "class F2 "
    "  destructor "
    "    F1._step := F1._step + 1; "
    "    F1._f2Step := F1._step; "
    "  end destructor; "
    "end F2; "
    "class Owner "
    "  private owned _f1: F1; "
    "  private owned _f2: F2; "
    "  public constructor(owned f1: F1, owned f2: F2) "
    "    _f1 := f1; "
    "    _f2 := f2; "
    "  end constructor; "
    "  destructor throw new AppError(); end destructor; "
    "end Owner; "
    "class Program "
    "  public static method Main(): int "
    "    var f1 := new F1(); "
    "    var f2 := new F2(); "
    "    var o := new Owner(move f1, move f2); "
    "    try "
    "      o.Dispose(); "
    "      return 10 * F1._f1Step + F1._f2Step; "
    "    catch "
    "      AppError => return 10 * F1._f1Step + F1._f2Step; "
    "    end; "
    "  end Main; "
    "end Program;";

BEZEL_TEST(codegen_throwing_destructor_1546,
           field_disposal_order_preserved_when_destructor_throws) {
    // F2 (declared second) disposed first (step 1), F1 (declared first)
    // disposed second (step 2): 10*2 + 1 == 21.
    BEZEL_EXPECT_EQ(run(kFieldDisposalOrderPreservedWhenDestructorThrows),
                    21);
}

// Regression: a NON-throwing destructor's owned-field disposal and base
// chain are unaffected by the local-unwind-target change (the ordinary,
// no-throw path never reaches the new `dtor.threw` block).
const char* kNonThrowingDestructorStillDisposesFieldAndBase =
    "module M; "
    "class Base "
    "  public static _baseRan: int := 0; "
    "  destructor Base._baseRan := Base._baseRan + 1; end destructor; "
    "end Base; "
    "class Field "
    "  public static _fieldRan: int := 0; "
    "  destructor Field._fieldRan := Field._fieldRan + 1; end destructor; "
    "end Field; "
    "class Owner: Base "
    "  private owned _f: Field; "
    "  public constructor(owned f: Field) _f := f; end constructor; "
    "  destructor end destructor; "
    "end Owner; "
    "class Program "
    "  public static method Main(): int "
    "    var f := new Field(); "
    "    var o := new Owner(move f); "
    "    o.Dispose(); "
    "    return 10 * Field._fieldRan + Base._baseRan; "
    "  end Main; "
    "end Program;";

BEZEL_TEST(codegen_throwing_destructor_1546,
           non_throwing_destructor_still_disposes_field_and_base) {
    BEZEL_EXPECT_EQ(run(kNonThrowingDestructorStillDisposesFieldAndBase), 11);
}
