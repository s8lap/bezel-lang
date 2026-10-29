// Spec-derived regression for issue #2357: the #2073 vacuous-using-var
// lint must NOT fire on `using var` / `await using var` over an iterator-
// frame return value (`src.GetEnumerator()` / `src.GetAsyncEnumerator()`
// / `d.GetOwningEnumerator()` / `src.GetAsyncEnumerator()` on a named
// `IAsyncEnumerable<T>` local). The returned `IAsyncEnumerator<T>` /
// `IEnumerator<T>` / `IOwningEnumerator<T>` IS a heap-allocated iterator
// frame with a real disposal obligation (vtable slot 1's `Dispose`) —
// `const` would silently drop the disposal and leak the iterator frame.
//
// Spec §28.9 (v0.235): "`IEnumerator<T>.Dispose()` (slot 1) releases any
//    resources the iterator was holding; the language does not require the
//    caller to call Dispose, but not calling it leaks resources on most
//    implementations."
// Spec §29.1 (v0.235): "`GetEnumerator()` returns `owned IEnumerator<T>`:
//    the caller takes ownership of the enumerator and must dispose it."
// Spec §30.7: "`GetAsyncEnumerator(token: CancellationToken
//    := CancellationToken.None): owned IAsyncEnumerator<T>`."
//
// STALE-AND-FIXED VERIFICATION
// ============================
// Issue #2357 was filed 2026-07-30; commit f2a8956 (fix #2385, #2451,
// 2026-07-31) added the iteration-protocol sentinel family
// (IEnumerable<T>/IEnumerator<T>/IOwningEnumerable<T>/IOwningEnumerator<T>
// and their async counterparts) to `rhsIsDisposableClassType`'s Path-2
// allowlist (`src/ownership/Ownership.cpp:4753-4757`), which is precisely
// the fix direction #2357's own text describes:
//   "add `IAsyncEnumerator`, `IOwningAsyncEnumerator`, `IEnumerator`,
//    `IOwningEnumerator` to `rhsIsDisposableClassType`'s Path-2 allowlist
//    (or teach it to recognize the iterator-frame shape more generally)."
//
// MECHANICAL DEMONSTRATION
// ========================
// Reverting the iteration-protocol allowlist arm (temporarily commenting
// lines 4753-4757 out) makes every test below that exercises an iterator-
// frame RHS go RED — the inert-RHS warning fires with exactly the message
// the issue reports:
//   "`using var` `en` is vacuous — the RHS is a value-typed/runtime-
//    managed value with no disposal obligation (§28.9); use `const x := …;`
//    for a fixed alias without the disposal hint (§28.10 / §21.1)"
// Restoring the allowlist brings them back to GREEN — the fix is in place
// on current HEAD. Tests below PIN each scenario the issue names plus the
// carve-out boundaries (#2451's "IList<int> still warns" control).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// True iff some WARNING diagnostic fires the inert-RHS vacuous-pin message
// for `name`. Mirrors the existing helper in vacuous_pin_lint_2073_test.cpp
// but inlined here so this file is a self-contained regression.
bool hasInertRhsWarning(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is vacuous") == std::string::npos) continue;
        if (d.message.find("RHS is a value-typed/runtime-managed") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// True iff some WARNING diagnostic fires the vacuous-borrow-pin message
// for `name`. Distinct from inert-RHS: pin extends nothing for a borrow
// that already outlives the enclosing scope (parameter, `this`, etc.).
bool hasVacuousBorrowPinWarning(const OwnershipResult& r,
                                const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("pins a source that already outlives") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// ============================================================================
// #2357 main repros — the issue's exact two shapes
// ============================================================================

// #2357 (sync): `using var e := src.GetEnumerator();` on a named
// `IEnumerable<int>` local from a compiler-generated `yield return`
// iterator method. The synthetic TypeRef built by `GetEnumerator`'s
// sema handler (`src/sema/ExprTypecheck_Calls.cpp:5117`) carries
// `astNode` ("IEnumerator") but no `declaration`, so Path 1 of
// `rhsIsDisposableClassType` falls through; without the #2385/#2451
// allowlist arm, Path 2 missed it too and the lint wrongly fired.
// // per §28.10, §29.1, #2385, #2451, #2357
BEZEL_TEST(vacuous_using_var_iterator_2357,
           using_var_getenumerator_no_warn_spec_29_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Gen; "
        "  public static method M(): int "
        "    var src := C.Gen(); "
        "    using var e := src.GetEnumerator(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "e"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "e"));
}

// #2357 (async): `await using var en := src.GetAsyncEnumerator();` on a
// named `IAsyncEnumerable<int>` local — the EXACT shape named in the
// issue body (modulo the optional `token` argument, which is also handled
// by the same `GetAsyncEnumerator` sema branch). The synthetic TypeRef
// built at `src/sema/ExprTypecheck_Calls.cpp:5355-5372` carries
// `astNode` ("IAsyncEnumerator") but no `declaration`; the same Path-1
// fallthrough + missing-Path-2 pattern applies. Note: the existing
// `vacuous_pin_lint_2073` #2385/#2451 block covers the SYNC shape
// (`GetEnumerator`) but NOT this ASYNC shape — #2357's specific repro
// was therefore still UNCOVERED by tests after the rework, making this
// file its first dedicated regression. FAILS WITHOUT THE
// ITERATION-PROTOCOL ALLOWLIST ARM (mechanically confirmed: temporarily
// commenting lines 4753-4757 of `src/ownership/Ownership.cpp` reproduces
// the inert-RHS warning message verbatim).
// // per §28.10, §29.1, §30.7, #2385, #2451, #2357
BEZEL_TEST(vacuous_using_var_iterator_2357,
           await_using_var_getasyncenumerator_no_warn_spec_29_1_30_7) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var src := C.Gen(); "
        "    await using var en := src.GetAsyncEnumerator(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "en"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "en"));
}

// #2357 sync twin with the EXPLICIT declared-type path — `using var
// e: IEnumerator<int> := src.GetEnumerator();`. #2451's rework 4
// (commit f2a8956, `src/ownership/Ownership.cpp:4526-4527`) made the
// classifier prefer the explicit declared type over the bare initializer
// expression's own type, specifically so coercing bindings through the
// iteration-protocol sentinels do not regress the classification. Test
// pins the declared-type path so a future regression that drops the
// `ld.type ? ctx_.resolvedType(*ld.type) : …` precedence (or that
// re-keys on `ld.initializer` only) is caught.
// // per §28.10, #2451 (rework 4), #2357
BEZEL_TEST(vacuous_using_var_iterator_2357,
           using_var_getenumerator_declared_type_no_warn_spec_29_1_2451) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end Gen; "
        "  public static method M(): int "
        "    var src := C.Gen(); "
        "    using var e: IEnumerator<int> := src.GetEnumerator(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "e"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "e"));
}

// #2357 async twin with explicit declared-type path. Same
// precedence-proof rationale as the sync version above.
// // per §28.10, §29.1, §30.7, #2451 (rework 4), #2357
BEZEL_TEST(vacuous_using_var_iterator_2357,
           await_using_var_getasyncenumerator_declared_type_no_warn_spec_29_1_30_7_2451) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var src := C.Gen(); "
        "    await using var en: IAsyncEnumerator<int> := src.GetAsyncEnumerator(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "en"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "en"));
}

// Owning-iterator variant: `d.GetOwningEnumerator()` on the
// `IOwningEnumerable<T>` returned by `OwningArray<T>.Drain()`. The
// owning enumerator is the same iterator-frame shape — §29.1's "owned
// transfer" rule applies to the `MoveNext`+`TakeCurrent` interface.
// Already exercised by #2451's test (vacuous_pin_lint_2073_test.cpp's
// `owning_enumerator_using_var_no_warn_spec_29_1_2451`), included here
// for self-containment so the regression file covers all four
// get-enumerator forms (#2357's "GetEnumerator / GetAsyncEnumerator /
// GetOwningEnumerator / GetOwningAsyncEnumerator" family).
// // per §28.10, §29.1, stdlib §4.5/§4.13, #2385, #2451, #2357
BEZEL_TEST(vacuous_using_var_iterator_2357,
           using_var_getowningenumerator_no_warn_spec_29_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class C "
        "  public static method M(): int "
        "    using var arr := new OwningArray<Cell>(); "
        "    arr.Add(new Cell()); "
        "    using var d := arr.Drain(); "
        "    using var e := d.GetOwningEnumerator(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "e"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "e"));
}

// ============================================================================
// Controls — make sure the fix does not MASK other warnings it should
// not affect. Pin both the negative (genuinely-vacuous-RHS still warns)
// and positive (genuinely-non-vacuous-RHS still silent) sides.
// ============================================================================

// Control (negative): `using var x := pureFunc();` where pureFunc
// returns `int` — the original §28.10 vacuous case. MUST still warn,
// proving the iteration-protocol allowlist arm is scoped to the eight
// head names and does not blanket-suppress the inert-RHS check.
// // per §28.10, #2073, #2357
BEZEL_TEST(vacuous_using_var_iterator_2357,
           pure_int_func_still_warns_control_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method PureFunc(): int return 42; end PureFunc; "
        "  public static method M(): int "
        "    using var x := C.PureFunc(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
}

// Control (positive): `using var x := new SomeDisposable();` for a
// disposable class type — the canonical §28.10 pattern. MUST NOT warn.
// // per §28.10, §28.11, #2073 (rework 2), #2357
BEZEL_TEST(vacuous_using_var_iterator_2357,
           new_disposable_class_no_warn_control_spec_28_10_28_11) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Cell; "
        "class C "
        "  public static method M(): int "
        "    using var x := new Cell(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

// Control (carve-out boundary): `IList<int>` is a DIFFERENT
// declaration-less sentinel (the array-backed, unwrapped protocol per
// iterator_native_test.cpp's `ilist_local_unwrapped_control`) that is
// deliberately EXCLUDED from `rhsIsDisposableClassType` — only the
// eight iteration-protocol names were added. A `using var` pinning a
// borrowing `Array<int>` through `IList<int>` remains genuinely
// vacuous (no fresh wrapper is allocated for IList) and must still
// warn, proving the fix is scoped to the exact head-name set.
// // per §28.10, §29.1, stdlib §4.12/§4.13, #2451, #2357
BEZEL_TEST(vacuous_using_var_iterator_2357,
           ilist_sentinel_still_warns_carveout_boundary_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method M(): int "
        "    using var a := new Array<int>(); "
        "    var v := a; "  // a borrowing alias: spec v0.350 §28.10 rejects the owned `a`
        "    using var seq: IList<int> := v; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "seq"));
}

// Control (carve-out boundary): `IReadOnlyCollection<int>` — also a
// declaration-less sentinel (IReadOnlyCollection is in the
// builtinTypeArity list at TypeResolution_Names.cpp), but NOT one of
// the eight iteration-protocol names. Pinning a borrowing Array<T>
// through it must still warn, proving the iteration-protocol fix is
// scoped to the exact eight names (the ones that ACTUALLY own the
// enumerator and dispose at scope end per §29.1).
// // per §28.10, §29.1, stdlib §4.12/§4.13, #2385, #2451, #2357
BEZEL_TEST(vacuous_using_var_iterator_2357,
           ireadonlycollection_sentinel_still_warns_carveout_boundary_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method M(): int "
        "    using var a := new Array<int>(); "
        "    var v := a; "  // a borrowing alias: spec v0.350 §28.10 rejects the owned `a`
        "    using var seq: IReadOnlyCollection<int> := v; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "seq"));
}

}  // namespace
