// fix #2630 (codegen leak, §28.5 / §28.11 / stdlib §4.3 / §4.5, v0.262): a
// LOCAL (not field) whose STATIC type is a borrowing read-only list interface
// (`IReadOnlyList<T>`) but whose DYNAMIC backing is an OWNING `OwningArray<T>`
// leaks its elements AND its backing. Local sibling of #2608 (which closed the
// `owned`-field shape).
//
// ROOT CAUSE (LlvmBackend_Setup.cpp:14901 `collectionTypeOfLocal` +
// `registerUsingDisposal`'s collection arm): the local-disposal path gates
// on `isCollectionTypeName`, which rejects Named TypeRefs with a non-null
// `declaration` — the erased-interface heads `IReadOnlyList<T>`/`IList<T>`
// are declaration-less (ExprTypecheck.cpp `makeBuiltin`, §4.3) but their
// name is not in `common::isArrayBackedCollectionName`, so the local
// registers NO disposal and the elements are leaked. The FIELD free site
// (`emitOwnedFieldDisposals`, LlvmBackend_Disposal.cpp:873) handles this via
// `bz_array_free_dynamic`, which consults the disposer the OWNING backing
// stamped into its `bz_array_t` header at construction (#2608). The local
// path simply never consulted it.
//
// WHY THE FIX IS STRUCTURAL, NOT A WIDENING (load-bearing). The issue text
// asks whether to widen `collIsOwning` for the borrowing list interface
// family. That CANNOT work: per stdlib §4.3, `IReadOnlyList<T>` is the shared
// read-only ancestor of BOTH hierarchies — `Array<T>` borrows, `OwningArray<T>`
// owns — so a single static type carries EITHER backing with OPPOSITE correct
// element behavior. The fix must consult the DYNAMIC backing's header
// disposer, exactly as #2608 did for the field shape. `bz_array_free_dynamic`
// reads the disposer header; an untagged (borrowing) backing yields NULL and
// the dynamic call degenerates EXACTLY to `bz_array_free(arr, null)` — the
// call it replaces for the borrowing case, bit-for-bit unchanged.
//
// FIX (mirrors #2608, no new runtime surface):
// 1. New helper `Emitter::listInterfaceTypeOfLocal` (LlvmBackend_Setup.cpp):
//    the erased-interface twin of `collectionTypeOfLocal`, accepts
//    declaration-less `IReadOnlyList<T>`/`IList<T>`/`IOwningList<T>` and
//    rejects user-declared types implementing these interfaces (a
//    `class MyC: IReadOnlyList<T>` does not go through this path).
// 2. New branch in `Emitter::registerUsingDisposal`: when the local's
//    declared/inferred type is a list interface, register a
//    `d.collection = true` disposal, derive `collectionElemDisposer` via
//    `elementPolicyFor(elem, ifaceIsOwning)`, fall back to the
//    string-element release thunk for a borrowing string, and set
//    `collArrayDynamicFree = true` when no static disposer was derived AND
//    the static head is a borrowing list interface.
// 3. New `bool collArrayDynamicFree` flag on `Disposal`
//    (LlvmBackend_impl.hpp) wired by `emitDisposals` to call
//    `bz_array_free_dynamic` instead of `bz_array_free(arr, null)`.
// 4. `IOwningList<T>` is statically owning — it gets the static disposer
//    path (`elementPolicyFor(elem, owning=true)` yields a vtable thunk) and
//    is never routed through the dynamic path; one fewer indirection.
//
// OBSERVABILITY: each `Cell` counts net-live instances (ctor ++, dtor --) and
// Main returns `Cell.n`, so the exit code is the number of Cells still ALIVE.
// 0 == every Cell disposed EXACTLY ONCE — a leak drives it positive, a
// double-free drives it negative AND trips ASan. runAotChecked's LSan
// additionally flags the raw allocation leak (the bz_array_t and its element
// storage, totaling 216 bytes for the issue repro's 2-Cell / 1-Array shape).
//
// REGRESSION COVERAGE:
//   * (A) CARDINAL — `using var v := Make()` where Make returns
//     `owned IReadOnlyList<Cell>` produced from an `OwningArray<Cell>`.
//     Pre-fix: LSan "216 byte(s) leaked in 4 allocation(s)" (2 Cells +
//     bz_array_t + element storage), Cell.n = 2. Post-fix: Cell.n = 0.
//   * (B) BORROWING-BACKING UAF GUARD — `using var v: IReadOnlyList<Cell> := tmp`
//     where `tmp` is a borrowing `Array<Cell>`. The Cell is owned by an
//     enclosing `using var`; the local MUST NOT dispose it. A `collIsOwning`
//     widening would make this a double-free. Cell.n = 0.
//   * (C) IOwningList<T> CONTROL — `using var v := Make()` where Make returns
//     `owned IOwningList<Cell>` from `OwningArray<Cell>`. The statically-
//     owning interface keeps its static disposer (elementPolicyFor with
//     owning=true); never routed through the dynamic path. Cell.n = 0.
//   * (D) OWNING OWNED-PARAM PASS-THROUGH — `using var v := Receive(owned tmp)`
//     where Receive is `(owned p: IReadOnlyList<Cell>) => ...`. Tests the
//     `passThroughOwnedArgToInterface`/interface-typed-owned-arg boundary
//     from the local side (a distinct lowering path from the Cardinal case).
//     Cell.n = 0.
//   * (E) STRING ELEMENT RELEASE — `using var v: IReadOnlyList<string> :=
//     arrayOfStrings` where `arrayOfStrings` is `Array<string>`. The string
//     +1 release thunk must still fire through the local-disposal path
//     (the OTHER direction regression: the dynamic path is NOT taken for a
//     statically-known string element). 200 strings of length 3 ⇒ 600 mod
//     256 = 88.
//   * (F) OWNING LOCAL TO OWNING INTERFACE — `using var v: IOwningList<Cell>
//     := owned tmp` (the statically-owning interface). The static disposer
//     path runs (not the dynamic path); elements disposed via vtable.
//     Cell.n = 0.
//
// SPEC CITATION: §28.5 (Local disposal / heap collection ownership),
// §28.11 (Collection element ownership), stdlib §4.3 (How the borrowing and
// owning hierarchies relate — the shared read-only ancestors), stdlib §4.5
// (`OwningArray<T>`).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

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
// the skip is now LOUD — the row FAILS — instead of being absorbed by a
// guard that reports a clean pass.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

// ----------------------------------------------------------------------
// (A) CARDINAL: `using var v := Make()` where Make returns
// `owned IReadOnlyList<Cell>` produced from a concrete `OwningArray<Cell>`.
// When `v` goes out of scope the elements MUST be disposed (Cell.n = 0).
// Pre-fix: LSan "216 byte(s) leaked in 4 allocation(s)" and Cell.n = 2.
// ----------------------------------------------------------------------
const char* kOwningBackedLocal =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Make(): owned IReadOnlyList<Cell> "
    "    var tmp := new OwningArray<Cell>(); "
    "    tmp.Add(new Cell()); "
    "    tmp.Add(new Cell()); "
    "    return tmp; "
    "  end Make; "
    "  public static method Run(): int "
    "    using var v := C.Make(); "
    "    return v.Count; "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "
    "    return Cell.n mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (B) BORROWING-BACKING UAF GUARD: `using var v: IReadOnlyList<Cell> := tmp`
// where `tmp` is a borrowing `Array<Cell>`. The Cell is owned by an enclosing
// `using var`; the local MUST NOT dispose it. This is the case a
// `collIsOwning` head-name widening would break (double-free). Cell.n = 0.
// ----------------------------------------------------------------------
const char* kBorrowingBackedLocal =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Run(): int "
    "    using var c := new Cell(); "
    "    var tmp := new Array<Cell>(); "
    "    tmp.Add(c); "
    "    tmp.Add(c); "
    "    using var v: IReadOnlyList<Cell> := tmp; "
    "    return v.Count; "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "
    "    return Cell.n mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (C) IOwningList<T> CONTROL: `using var v := Make()` where Make returns
// `owned IOwningList<Cell>` from `OwningArray<Cell>`. The statically-owning
// interface keeps its static disposer (elementPolicyFor with owning=true);
// must NEVER be diverted onto the dynamic path nor disposed twice. Cell.n = 0.
// ----------------------------------------------------------------------
const char* kOwningListLocal =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Make(): owned IOwningList<Cell> "
    "    var tmp := new OwningArray<Cell>(); "
    "    tmp.Add(new Cell()); "
    "    tmp.Add(new Cell()); "
    "    return tmp; "
    "  end Make; "
    "  public static method Run(): int "
    "    using var v := C.Make(); "
    "    return v.Count; "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "
    "    return Cell.n mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (D) OWNING PASSED-PARAM TO LIST-INTERFACE PARAM — tests the
// `owned` arg → interface-typed `owned` parameter boundary from the local
// side (a distinct lowering path from (A), which is the direct return
// shape). Cell.n = 0.
// ----------------------------------------------------------------------
const char* kOwningBackedParam =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Sink "
    "  public method N(v: IReadOnlyList<Cell>): int "
    "    return v.Count; "
    "  end N; "
    "end Sink; "
    "class C "
    "  public static method Make(): owned IReadOnlyList<Cell> "
    "    var tmp := new OwningArray<Cell>(); "
    "    tmp.Add(new Cell()); "
    "    tmp.Add(new Cell()); "
    "    return tmp; "
    "  end Make; "
    "  public static method Run(): int "
    "    using var v := C.Make(); "
    "    var s := new Sink(); "
    "    return s.N(v); "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "
    "    return Cell.n mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (E) STRING ELEMENT RELEASE — the OTHER direction regression: a string-
// typed element yields a NON-null static disposer (stringElementReleaseThunk),
// so the dynamic path is NOT taken — the local must release the +1 per
// element of the borrowing `Array<string>` backing. Pre-#2603 this leaked
// one reference per element; #2603 fixed the field shape; this regression
// pins the local shape. 100 iterations × v[0].Length(3) ⇒ sink = 300,
// exit 300 mod 256 = 44.
// ----------------------------------------------------------------------
const char* kStringElementRelease =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      var arr := new Array<string>(); "
    "      arr.Add(\"abc\"); "
    "      arr.Add(\"abc\"); "
    "      using var v: IReadOnlyList<string> := arr; "
    "      sink := sink + v[0].Length; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (F) OWNING LOCAL TO OWNING INTERFACE — `using var v: IOwningList<Cell> :=
// owned tmp`. The static disposer path runs (not the dynamic path);
// elements disposed via vtable. Cell.n = 0.
// ----------------------------------------------------------------------
const char* kOwningLocalToOwningInterface =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Run(): int "
    "    var tmp := new OwningArray<Cell>(); "
    "    tmp.Add(new Cell()); "
    "    tmp.Add(new Cell()); "
    "    using var v: IOwningList<Cell> := owned tmp; "
    "    return v.Count; "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "
    "    return Cell.n mod 256; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2630 (A) — CARDINAL: a `using var` typed by an `IReadOnlyList<Cell>` and
// produced from an `OwningArray<Cell>` disposes its elements AND its
// backing. Pre-fix LSan flagged 216 byte(s) in 4 allocation(s) and
// Cell.n = 2 (elements never disposed, bz_array_t leaked).
// =====================================================================
BEZEL_TEST(codegen_erased_interface_local_disposal_2630,
           ireadonlylist_local_owning_backing_elements_disposed_spec_28_11) {
    JitResult r = runAotChecked(kOwningBackedLocal);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // LSan-clean (the #2630 leak flips this)
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0 — disposed exactly once
}

// =====================================================================
// #2630 (B) — BORROWING-BACKING UAF GUARD: a `using var` typed by an
// `IReadOnlyList<Cell>` whose dynamic backing is a borrowing `Array<Cell>`
// must NOT dispose its elements. The element is owned by an enclosing
// `using var`; a `collIsOwning` widening would double-free it.
// =====================================================================
BEZEL_TEST(codegen_erased_interface_local_disposal_2630,
           ireadonlylist_local_borrowing_backing_elements_not_disposed_spec_4_3) {
    JitResult r = runAotChecked(kBorrowingBackedLocal);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0 — no double-free
}

// =====================================================================
// #2630 (C) — IOwningList<T> CONTROL: the statically-owning interface
// keeps its static disposer (elementPolicyFor with owning=true); must
// never be diverted onto the dynamic path.
// =====================================================================
BEZEL_TEST(codegen_erased_interface_local_disposal_2630,
           iowninglist_local_static_owning_preserved_spec_28_11) {
    JitResult r = runAotChecked(kOwningListLocal);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0
}

// =====================================================================
// #2630 (D) — OWNING PASSED-PARAM TO LIST-INTERFACE PARAM: tests the
// interface-typed-owned-arg boundary from the local side (a distinct
// lowering path from the Cardinal (A) return shape).
// =====================================================================
BEZEL_TEST(codegen_erased_interface_local_disposal_2630,
           ireadonlylist_local_owned_arg_to_iface_param_disposed_spec_28_11) {
    JitResult r = runAotChecked(kOwningBackedParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0
}

// =====================================================================
// #2630 (E) — STRING ELEMENT RELEASE: a string-typed element yields a
// NON-null static disposer (stringElementReleaseThunk), so the dynamic
// path is NOT taken — the local must release the +1 per element of the
// borrowing `Array<string>` backing. The OTHER direction regression: the
// stdlib admits non-owning backings for `IReadOnlyList<T>`, and the
// string +1 release must still fire through the static path.
// =====================================================================
BEZEL_TEST(codegen_erased_interface_local_disposal_2630,
           ireadonlylist_string_local_static_release_preserved_spec_28_11) {
    JitResult r = runAotChecked(kStringElementRelease);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);   // 100 × 3 mod 256 = 300 mod 256 = 44
}

// =====================================================================
// #2630 (F) — OWNING LOCAL TO OWNING INTERFACE: `using var v:
// IOwningList<Cell> := owned tmp`. The static disposer path runs (not
// the dynamic path); elements disposed via vtable. Cell.n = 0.
// =====================================================================
BEZEL_TEST(codegen_erased_interface_local_disposal_2630,
           iowninglist_local_owned_assignment_static_owning_spec_28_11) {
    JitResult r = runAotChecked(kOwningLocalToOwningInterface);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0
}
