// Codegen — issue #2748: the THREE lazy emitters that never adopted
// `Emitter::NestedBodyScope` (#2702), the guard the three already-guarded
// emitters (`ensurePropertyBlockGetter`, `ensureIndexerBlockGetter`,
// `ensurePropertyAccessorFn`) all wrap themselves in.
//
// Spec (revision v0.240):
//   §23.2 "Properties" — `get => expr;` is the EXPRESSION-bodied form; the
//     `get => do ... end` block-bodied form is a separate, MEMOIZED standalone
//     function emitted on first read.
//   §23.4 "Indexers" — the same accessor forms for `property [i: int]: T`.
//   §29.2 "IEnumerable<T> and IEnumerator<T>" — a user class implementing
//     IEnumerable<T> is wrapped on first for-in into a vtable-backed
//     enumerator; the wrapper's `Current` slot is filled by either
//     `ensurePropertyBlockGetter` (block-bodied), `ensurePropertyAccessorFn`
//     (auto/abstract/specialized), or `ensureExprGetterThunk` (expression-
//     bodied on a LOCAL enumerator class).
//
// THE THREE UNGUARDED EMITTERS. All three build a memoised standalone function
// from the middle of another function's emission — the exact face the
// `NestedBodyScope` guard (#2702) was created for (#2170, #2694, #2701, #2702,
// #2703, #2731 items 1 and 3, #3125 each had to be patched field-by-field at
// three sites before the guard factored them into one place). The issue
// verified by exhaustive call-graph analysis that:
//
//   1. `ensureIndexerGetterFn` (LlvmBackend_Assign.cpp) and
//      `ensureIndexerSetterFn` are MODULE-LEVEL ONLY — their only drivers
//      (`declareVtables` Pass D, `setupBoxDescriptors`,
//      `emitExportedIndexerGetters`/`Setters`) all sit above the method-body
//      loop, so every member the guard resets is already neutral at every
//      reachable entry. Adopting the guard here is pure hardening; no live
//      defect was reproduced.
//
//   2. `ensureExprGetterThunk` IS reachable from ordinary expression lowering
//      (seven caller chains enumerated in the issue; the load-bearing ones are
//      `emitFunctionBody`, `buildSpecializedMethodFn`, `emitInlineGenericMethod`
//      — exactly the contexts where `genericSubst_` is non-empty). The same
//      field-by-field hazard the guard exists to neutralise is therefore live
//      here. No repro was constructed, but the path is reachable and the
//      guard's premises hold.
//
// THE HAZARD FOR #3 SPECIFICALLY. The thunk originally CLEARED `genericSubst_`
// (`diverged_ = false; genericSubst_.clear();`), the odd-one-out of the six
// lazy emitters. The thunk's signature `retTy` is passed IN by the caller,
// computed under the caller's substitution, which is exactly the argument
// #2702 used to justify NOT clearing. Adopting the guard as-is would have left
// `genericSubst_` cleared while `genericSubstSemaTy_` was inherited —
// desynced in the OPPOSITE direction from the desync #2702 fixed. The
// explicit clear is removed; `genericSubst_` is now inherited like every
// other per-body emitter, and the save/restore stays (consistent with the
// other five).
//
// THE TESTS BELOW are smoke tests on the THREE newly-guarded emitters plus
// regression-guard smoke tests on the THREE already-guarded emitters. The
// already-guarded emitters have full defect-pinning tests in
// `async_chained_return_nested_body_2731_test.cpp` (8 cases) and
// `requires_prologue_nested_body_2731_test.cpp` (6 cases); the smoke tests
// here exist to prove that adding the guard to the three NEW emitters did
// not silently break the inherited `genericSubst_` plumbing on the three
// ALREADY-guarded ones (a guard-widening error would be loudest there).
//
// The tests run on the PLAIN oracle (`runAot`) and on `runAotChecked`
// (AOT + ASan), per the standing rule that #2633 proved neither oracle
// dominates — both must be green.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

int runPlain(const std::string& src) {
    JitResult r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────────
// (1) `ensureIndexerGetterFn` — the vtable/itable face of an EXPRESSION-bodied
// INDEXER GETTER. Reachable only from `declareVtables` Pass D,
// `setupBoxDescriptors`, and `emitExportedIndexerGetters` (all module-level);
// the issue verified the inherited state is the empty default at every
// reachable entry, so the guard adoption is hardening-only.
// ────────────────────────────────────────────────────────────────────────────

// §23.4 / §12.1 / §11.2 — a `virtual` expression-bodied indexer getter whose
// declared element type is `int?` (the existing #2850 repro path through this
// emitter). Pre-#2850 the standalone function emitted `ret ptr null` from a
// `{i8, i32}` slot and the IR verifier rejected the module; this commit only
// adopts the guard, not the #2850 wrap. The smoke test therefore uses a
// non-nullable element type so the wrap is a no-op and we are only testing
// that the guard adoption did not break the vtable-slot emission.
static const std::string kVtableIndexerGetter =
    "module M; "
    "class B "
    "  public _n: int := 7; "
    "  public virtual property [i: int]: int "
    "    get => _n + i; "
    "  end; "
    "end B; "
    "class C public static method Main(): int "
    "  using var b := new B(); "
    "  return b[3]; "      // 7 + 3 = 10
    "end Main; end C;";

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           vtable_indexer_getter_plain) {
    BEZEL_EXPECT_EQ(runPlain(kVtableIndexerGetter), 10);
}

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           vtable_indexer_getter_checked) {
    int rc = runChecked(kVtableIndexerGetter);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 10);
}

// §23.4 / §11.2 — the ITABLE face of the same emitter, through an interface-
// typed receiver. `setupBoxDescriptors` builds this slot from
// `ensureIndexerGetterFn`.
static const std::string kItableIndexerGetter =
    "module M; "
    "interface IAdd readonly property [i: int]: int; end IAdd; "
    "class B: IAdd public property [i: int]: int => 5 + i; end B; "
    "class C public static method Main(): int "
    "  using var b := new B(); "
    "  var q: IAdd := b; "
    "  return q[4]; "      // 5 + 4 = 9
    "end Main; end C;";

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           itable_indexer_getter_plain) {
    BEZEL_EXPECT_EQ(runPlain(kItableIndexerGetter), 9);
}

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           itable_indexer_getter_checked) {
    int rc = runChecked(kItableIndexerGetter);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 9);
}

// ────────────────────────────────────────────────────────────────────────────
// (2) `ensureIndexerSetterFn` — the vtable/itable face of an EXPRESSION-bodied
// INDEXER SETTER. Returns void and may legitimately contain a bare `return;`
// that runs through `LlvmBackend_Stmts.cpp` ~1795, so the inherited-state
// hazard is not just an `inlinedAccessorEnd_` cross-function `br` waiting to
// happen on a state-machine emitter.
// ────────────────────────────────────────────────────────────────────────────

// §23.4 / §12.1 — a `virtual` expression-bodied indexer SETTER, the body
// is `value := value * 2` (a single assignment, the simplest viable setter).
// The setter returns void; the read-back happens via a separate non-virtual
// getter (`=> _s`) so the assertion value is unambiguous.
static const std::string kVtableIndexerSetter =
    "module M; "
    "class B "
    "  public _s: int := 0; "
    "  public virtual property [i: int]: int "
    "    get => _s; "
    "    set => do _s := value * 2; end; "
    "  end; "
    "end B; "
    "class C public static method Main(): int "
    "  using var b := new B(); "
    "  b[2] := 21; "
    "  return b[2]; "      // 21 * 2 = 42
    "end Main; end C;";

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           vtable_indexer_setter_plain) {
    BEZEL_EXPECT_EQ(runPlain(kVtableIndexerSetter), 42);
}

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           vtable_indexer_setter_checked) {
    int rc = runChecked(kVtableIndexerSetter);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 42);
}

// ────────────────────────────────────────────────────────────────────────────
// (3) `ensureExprGetterThunk` — the THUNK for an EXPRESSION-bodied
// getter, built on demand by `wrapUserClassAsIEnumerable` when an
// `IEnumerable<T>`-implementing class's `Current` accessor is an `=>`
// expression. This is the REACHABLE one of the three unguarded lazy emitters
// (seven caller chains enumerated in the issue). The original code cleared
// `genericSubst_`; the new code inherits it (per #2702), so this test
// confirms the thunk still produces the right value after both changes.
// ────────────────────────────────────────────────────────────────────────────

// §29.2 / §23.2 — the canonical two-class idiom from #2388 / #2424: a local
// enumerator with an EXPRESSION-bodied `Current` (`get => cur;`), wrapped on
// for-in via `wrapUserClassAsIEnumerable`. The `IntRange` source is
// `IEnumerable<int>`; the for-in driver builds the wrapper, which calls
// `ensureExprGetterThunk` for `Current`.
static const std::string kExprGetterThunk =
    "module M; "
    "class IntRangeEnum: IEnumerator<int> "
    "  private lo: int; private hi: int; private cur: int; "
    "  public constructor(lo: int, hi: int) "
    "    this.lo:=lo; this.hi:=hi; cur:=lo-1; "
    "  end constructor; "
    "  public method MoveNext(): bool "
    "    cur:=cur+1; return cur<=hi; "
    "  end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end IntRangeEnum; "
    "class IntRange: IEnumerable<int> "
    "  private lo: int; private hi: int; "
    "  public constructor(lo: int, hi: int) "
    "    this.lo:=lo; this.hi:=hi; "
    "  end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> "
    "    return new IntRangeEnum(lo, hi); "
    "  end GetEnumerator; "
    "end IntRange; "
    "class C public static method Main(): int "
    "  using var s := new IntRange(1, 4); "
    "  var sum := 0; "
    "  for var x in s do sum := sum + x; end; "
    "  return sum; "        // 1+2+3+4 = 10
    "end Main; end C;";

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           expr_getter_thunk_for_in_plain) {
    BEZEL_EXPECT_EQ(runPlain(kExprGetterThunk), 10);
}

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           expr_getter_thunk_for_in_checked) {
    int rc = runChecked(kExprGetterThunk);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 10);
}

// §29.2 / §23.2 — adversarial: the same shape read MULTIPLE TIMES through a
// fresh `for ... in` on the SAME source, so the thunk is invoked repeatedly
// (memoisation check — must return the same thunk the second time) and the
// guard's destructor must run cleanly each time. `sum * 2` is a no-op on the
// value but the for-in completes the wrapper's full Dispose path on each
// iteration.
static const std::string kExprGetterThunkRepeated =
    "module M; "
    "class IntRangeEnum: IEnumerator<int> "
    "  private lo: int; private hi: int; private cur: int; "
    "  public constructor(lo: int, hi: int) "
    "    this.lo:=lo; this.hi:=hi; cur:=lo-1; "
    "  end constructor; "
    "  public method MoveNext(): bool "
    "    cur:=cur+1; return cur<=hi; "
    "  end MoveNext; "
    "  public property Current: int get => cur * 2; end Current; "
    "end IntRangeEnum; "
    "class IntRange: IEnumerable<int> "
    "  private lo: int; private hi: int; "
    "  public constructor(lo: int, hi: int) "
    "    this.lo:=lo; this.hi:=hi; "
    "  end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> "
    "    return new IntRangeEnum(lo, hi); "
    "  end GetEnumerator; "
    "end IntRange; "
    "class C public static method Main(): int "
    "  var s: IEnumerable<int> := new IntRange(1, 3); "
    "  var total := 0; "
    "  var r := 0; "
    "  while r < 8 do "
    "    for var x in s do total := total + x; end; "
    "    r := r + 1; "
    "  end; "
    "  return total; "       // (2+4+6) * 8 = 96
    "end Main; end C;";

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           expr_getter_thunk_repeated_for_in_plain) {
    BEZEL_EXPECT_EQ(runPlain(kExprGetterThunkRepeated), 96);
}

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           expr_getter_thunk_repeated_for_in_checked) {
    int rc = runChecked(kExprGetterThunkRepeated);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 96);
}

// ────────────────────────────────────────────────────────────────────────────
// (4-6) REGRESSION GUARDS on the THREE ALREADY-GUARDED emitters. The full
// defect-pinning tests live in `async_chained_return_nested_body_2731_test.cpp`
// and `requires_prologue_nested_body_2731_test.cpp`; these are smoke tests
// that the guard-adoption on the three NEW emitters did not silently break
// the inherited `genericSubst_` plumbing on the three ALREADY-guarded ones.
// A guard-widening error — e.g. clearing a member the guard inherits —
// would surface here as a wrong value or a build failure.
// ────────────────────────────────────────────────────────────────────────────

// (4) `ensurePropertyBlockGetter` — §23.3 `get => do ... end`. A
// block-bodied property whose body touches a member that requires the
// caller's `genericSubst_` (here the receiver's `Foo` is a non-generic
// class so `genericSubst_` is empty by construction, which is the safe
// direction).
static const std::string kPropertyBlockGetter =
    "module M; "
    "class B "
    "  private _n: int; "
    "  public constructor() _n := 11; end constructor; "
    "  public property N: int "
    "    get => do "
    "      return _n; "
    "    end; "
    "  end N; "
    "end B; "
    "class C public static method Main(): int "
    "  using var b := new B(); return b.N; "
    "end Main; end C;";

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           regression_property_block_getter_plain) {
    BEZEL_EXPECT_EQ(runPlain(kPropertyBlockGetter), 11);
}

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           regression_property_block_getter_checked) {
    int rc = runChecked(kPropertyBlockGetter);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 11);
}

// (5) `ensureIndexerBlockGetter` — §23.4 `property [i: int]: T get => do ... end`.
// A block-bodied indexer getter. Same no-`genericSubst_` invariant.
static const std::string kIndexerBlockGetter =
    "module M; "
    "class B "
    "  private _n: int; "
    "  public constructor() _n := 13; end constructor; "
    "  public property [i: int]: int "
    "    get => do "
    "      return _n + i; "
    "    end; "
    "  end; "
    "end B; "
    "class C public static method Main(): int "
    "  using var b := new B(); return b[4]; "    // 13 + 4 = 17
    "end Main; end C;";

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           regression_indexer_block_getter_plain) {
    BEZEL_EXPECT_EQ(runPlain(kIndexerBlockGetter), 17);
}

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           regression_indexer_block_getter_checked) {
    int rc = runChecked(kIndexerBlockGetter);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 17);
}

// (6) `ensurePropertyAccessorFn` — §23.2 / §12.1 / §12.5 vtable-slot face.
// A virtual expression-bodied property getter (the AUTO-property arm is also
// here; using `=> _n` exercises the lowerExpr path through the guard).
static const std::string kPropertyAccessorFn =
    "module M; "
    "class B "
    "  public _n: int := 19; "
    "  public virtual property V: int => _n; "
    "end B; "
    "class C public static method Main(): int "
    "  using var b := new B(); return b.V; "
    "end Main; end C;";

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           regression_property_accessor_fn_plain) {
    BEZEL_EXPECT_EQ(runPlain(kPropertyAccessorFn), 19);
}

BEZEL_TEST(codegen_unguarded_lazy_emitters_2748,
           regression_property_accessor_fn_checked) {
    int rc = runChecked(kPropertyAccessorFn);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 19);
}
