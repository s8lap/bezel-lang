// fix #2608 (codegen leak, §28.5 / §28.11 / stdlib §4.3 / §4.5, v0.236): an
// `owned`-modifier field whose STATIC type is a BORROWING read-only list
// interface (`IReadOnlyList<T>`) but whose DYNAMIC backing is an OWNING
// `OwningArray<T>` leaks its class elements. #2603 closed the backing-storage
// leak for this shape (freed via `bz_array_free`); this is the residual
// ELEMENT-disposal half.
//
// ROOT CAUSE (LlvmBackend_Disposal.cpp:602 / LlvmBackend_Setup.cpp:15595): the
// element-ownership flag `collIsOwning` is derived from the field's STATIC head
// name. For `IReadOnlyList` it is FALSE, so `elementPolicyFor(elem, false)`
// yields ClassBorrow with `disposerThunk == nullptr` and `bz_array_free` is
// called with a NULL element disposer — the owned elements are never disposed.
//
// WHY A HEAD-NAME WIDENING CANNOT FIX IT (the load-bearing point). Per stdlib
// §4.3, "The two hierarchies share `IEnumerable<T>`, `IReadOnlyCollection<T>`,
// and `IReadOnlyList<T>` as common ancestors", and §4.3's own example upcasts
// BOTH `Array<Connection>` and `OwningArray<Connection>` to
// `IReadOnlyList<Connection>`. So ONE static field type legitimately carries
// EITHER backing, with OPPOSITE correct behavior:
//   * backing `OwningArray<T>` — elements are owned; they MUST be disposed
//     (test A/C/D below; pre-fix they leaked).
//   * backing `Array<T>`       — elements are borrowed; they MUST NOT be
//     disposed (test B/E below; correct pre-fix).
// Marking the family owning (the `collIsOwning` widening used for IOwningList
// by #2606) would fix the first and turn the second into a DOUBLE-FREE / UAF.
// Element ownership here is a property of the DYNAMIC backing, not of the
// static type, so the fix has to be structural rather than another widening.
//
// FIX: record the owning collection's element disposer in the `bz_array_t`
// header at CONSTRUCTION — the one place the concrete type is known
// (`bz_array_set_elem_disposer`, emitted from the collection-construction site
// in LlvmBackend_Exprs2.cpp for an `Owning`-prefixed array-backed collection) —
// and, at an erased-interface owned-field free site that derived NO static
// element disposer, free via the new `bz_array_free_dynamic`, which uses the
// header's disposer. A BORROWING backing is never tagged, so its header
// disposer is NULL and `bz_array_free_dynamic` degenerates EXACTLY to the
// `bz_array_free(val, nullptr)` it replaces — the borrowing case is bit-for-bit
// unchanged and this path can never over-dispose. Every pre-existing free site
// still passes its statically-derived disposer and ignores the header field.
//
// OBSERVABILITY: each `Cell` counts net-live instances (ctor ++, dtor --) and
// Main returns `Cell.n`, so the exit code is the number of Cells still ALIVE.
// 0 == every Cell disposed EXACTLY ONCE — a leak drives it positive, a
// double-free drives it negative AND trips ASan. runAotChecked's LSan
// additionally flags the raw allocation leak.
//
// REGRESSION COVERAGE:
//   * (A) CARDINAL — `owned _arr: IReadOnlyList<Cell>` populated from a
//     concrete `OwningArray<Cell>` producer. Pre-fix: LSan "32 byte(s) leaked
//     in 2 allocation(s)" (the two Cells) and Cell.n = 2. Post-fix: Cell.n = 0.
//   * (B) BORROWING-BACKING UAF GUARD — the SAME field type holding a borrowing
//     `Array<Cell>` whose element is owned by an enclosing `using var`. The
//     element must NOT be disposed by the field. Correct pre-fix; a
//     `collIsOwning` widening would make this a double-free. Cell.n = 0.
//   * (C) REASSIGNMENT / EXACTLY-ONCE — the owning-backed field reassigned
//     twice (the drop-on-overwrite path, lowerOwnedCollectionFieldStore). Each
//     displaced backing's elements are disposed exactly once. Cell.n = 0.
//   * (D) CAPTURED PRIMARY-CTOR-PARAM — `class Holder(owned v:
//     IReadOnlyList<Cell>)`, the `posParam` arm of ownedCollectionFieldType (a
//     DISTINCT code path from the FieldDecl arm, §22.10). Cell.n = 0.
//   * (E) BORROWING REASSIGNMENT UAF GUARD — borrowing backing REASSIGNED, so
//     the displaced borrowing backing goes through the drop-on-overwrite path
//     with a still-live borrowed element. Cell.n = 0, no ASan report.
//   * (F) owned `IList<string>` CONTROL (#2603) — the string-element release
//     gate must still fire through the STATIC path (elemDisp non-null, so the
//     dynamic path is not taken). exit 44.
//   * (G) owned `IOwningList<Cell>` CONTROL (#2606) — the statically-owning
//     interface keeps its static disposer. Cell.n = 0.
//   * (H) owned `OwningArray<Cell>` CONTROL — the concrete owning array path is
//     untouched. Cell.n = 0.
//
// SPEC CITATION: §28.5 (Owned fields), §28.11 (Collection element ownership),
// stdlib §4.3 (How the borrowing and owning hierarchies relate — the shared
// read-only ancestors), stdlib §4.5 (`OwningArray<T>`).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 8 absorbing rows used to
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

// ----------------------------------------------------------------------
// (A) CARDINAL: owned IReadOnlyList<Cell> field with an OWNING backing. The
// concrete OwningArray<Cell> producer is upcast-stored into the interface
// field; when the holder is disposed the elements must be disposed too.
// Pre-fix: 32 byte(s) leaked in 2 allocation(s), Cell.n = 2.
// ----------------------------------------------------------------------
const char* kOwningBackedElements =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _arr: IReadOnlyList<Cell> := new OwningArray<Cell>(); "
    "  public method Fill(): int "
    "    var tmp := new OwningArray<Cell>(); "
    "    tmp.Add(new Cell()); "
    "    tmp.Add(new Cell()); "
    "    _arr := tmp; "          // upcast store -> lowerOwnedCollectionFieldStore
    "    return _arr.Count; "    // 2
    "  end Fill; "
    "end Holder; "
    "class C "
    "  public static method Run(): int "
    "    using var h := new Holder(); "
    "    return h.Fill(); "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "    // h disposed here -> cells disposed
    "    return Cell.n mod 256; "  // 0 iff elements disposed exactly once
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (B) BORROWING-BACKING UAF GUARD: the SAME static field type holding a
// BORROWING Array<Cell>. The element is owned by an enclosing `using var` and
// must NOT be disposed by the field. This is the case a `collIsOwning`
// head-name widening would break (double-free). Cell.n = 0.
// ----------------------------------------------------------------------
const char* kBorrowingBackedElements =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _arr: IReadOnlyList<Cell> := new Array<Cell>(); "
    "  public method Fill(borrowed: Cell): int "
    "    var tmp := new Array<Cell>(); "
    "    tmp.Add(borrowed); "
    "    _arr := tmp; "
    "    return _arr.Count; "    // 1
    "  end Fill; "
    "end Holder; "
    "class C "
    "  public static method Run(): int "
    "    using var c := new Cell(); "
    "    using var h := new Holder(); "
    "    return h.Fill(c); "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "
    "    return Cell.n mod 256; "  // 0 — c disposed once by its own `using var`
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (C) REASSIGNMENT / EXACTLY-ONCE: the owning-backed interface field is
// reassigned twice, so two displaced backings go through the drop-on-overwrite
// path (lowerOwnedCollectionFieldStore). 3 cells created, 3 disposed exactly
// once (a double-free would drive Cell.n negative and trip ASan). Cell.n = 0.
// ----------------------------------------------------------------------
const char* kOwningBackedReassign =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _arr: IReadOnlyList<Cell> := new OwningArray<Cell>(); "
    "  public method FillAndReset(): int "
    "    var tmp := new OwningArray<Cell>(); "
    "    tmp.Add(new Cell()); "
    "    tmp.Add(new Cell()); "
    "    _arr := tmp; "            // drop empty backing; store 2-cell backing
    "    var tmp2 := new OwningArray<Cell>(); "
    "    tmp2.Add(new Cell()); "
    "    _arr := tmp2; "           // drop 2-cell backing -> both disposed here
    "    return _arr.Count; "      // 1
    "  end FillAndReset; "
    "end Holder; "
    "class C "
    "  public static method Run(): int "
    "    using var h := new Holder(); "
    "    return h.FillAndReset(); "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "
    "    return Cell.n mod 256; "  // 0 — 3 created, 3 disposed exactly once
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (D) CAPTURED PRIMARY-CTOR-PARAM: `class Holder(owned v: IReadOnlyList<Cell>)`
// captures the `owned` param as a synthesized field (§22.10) — the `posParam`
// arm of ownedCollectionFieldType, a DISTINCT path from the FieldDecl arm. The
// caller hands over a concrete OwningArray<Cell>; the callee sees only the
// erased interface, so this is the shape no static analysis can classify.
// Cell.n = 0.
// ----------------------------------------------------------------------
const char* kOwningBackedCtorParam =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder(owned v: IReadOnlyList<Cell>) "
    "  public method N(): int return v.Count; end N; "
    "end Holder; "
    "class C "
    "  public static method Run(): int "
    "    var tmp := new OwningArray<Cell>(); "
    "    tmp.Add(new Cell()); "
    "    tmp.Add(new Cell()); "
    "    using var h := new Holder(move tmp); "
    "    return h.N(); "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "
    "    return Cell.n mod 256; "  // 0
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (E) BORROWING REASSIGNMENT UAF GUARD: a BORROWING backing displaced through
// the drop-on-overwrite path while its element is still live and owned by an
// enclosing `using var`. The displaced backing must free its storage WITHOUT
// disposing the borrowed element. Cell.n = 0, no ASan report.
// ----------------------------------------------------------------------
const char* kBorrowingBackedReassign =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _arr: IReadOnlyList<Cell> := new Array<Cell>(); "
    "  public method Swap(a: Cell, b: Cell): int "
    "    var t1 := new Array<Cell>(); "
    "    t1.Add(a); "
    "    _arr := t1; "
    "    var t2 := new Array<Cell>(); "
    "    t2.Add(a); "
    "    t2.Add(b); "
    "    _arr := t2; "            // displaced t1 must NOT dispose `a`
    "    return _arr.Count; "     // 2
    "  end Swap; "
    "end Holder; "
    "class C "
    "  public static method Run(): int "
    "    using var c1 := new Cell(); "
    "    using var c2 := new Cell(); "
    "    using var h := new Holder(); "
    "    return h.Swap(c1, c2); "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "
    "    return Cell.n mod 256; "  // 0
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (F) owned IList<string> CONTROL (#2603): a string element yields a NON-null
// static disposer (stringElementReleaseThunk), so the dynamic path is NOT
// taken and the #2603 behavior must be bit-for-bit preserved. exit 44.
// ----------------------------------------------------------------------
const char* kOwnedIListStringControl =
    "module M; "
    "class Holder "
    "  private owned _arr: IList<string> := new Array<string>(); "
    "  public method AddIt(): int "
    "    _arr.Add(\"abc\"); "
    "    _arr[0] := \"xyz\"; "
    "    return _arr[0].Length; "  // 3
    "  end AddIt; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      using var h := new Holder(); "
    "      sink := sink + h.AddIt(); "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "  // 300 mod 256 = 44
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (G) owned IOwningList<Cell> CONTROL (#2606): the statically-OWNING list
// interface keeps its static disposer (collIsOwning=true), so it must not be
// diverted onto the dynamic path nor disposed twice. Cell.n = 0.
// ----------------------------------------------------------------------
const char* kOwnedIOwningListControl =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _arr: IOwningList<Cell> := new OwningArray<Cell>(); "
    "  public method Fill(): int "
    "    var tmp := new OwningArray<Cell>(); "
    "    tmp.Add(new Cell()); "
    "    tmp.Add(new Cell()); "
    "    _arr := tmp; "
    "    return _arr.Count; "
    "  end Fill; "
    "end Holder; "
    "class C "
    "  public static method Run(): int "
    "    using var h := new Holder(); "
    "    return h.Fill(); "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "
    "    return Cell.n mod 256; "  // 0
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (H) owned OwningArray<Cell> CONTROL: the concrete owning-array field path
// (collIsOwning via isOwningPrefixedName) must be unaffected by the
// construction-site tagging. Cell.n = 0.
// ----------------------------------------------------------------------
const char* kOwnedOwningArrayControl =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _arr: OwningArray<Cell> := new OwningArray<Cell>(); "
    "  public method Fill(): int "
    "    _arr.Add(new Cell()); "
    "    _arr.Add(new Cell()); "
    "    return _arr.Count; "
    "  end Fill; "
    "end Holder; "
    "class C "
    "  public static method Run(): int "
    "    using var h := new Holder(); "
    "    return h.Fill(); "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "
    "    return Cell.n mod 256; "  // 0
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2608 (A) — CARDINAL: an owned IReadOnlyList<Cell> field backed by an
// OwningArray<Cell> disposes its elements. Pre-fix LSan flagged 32 byte(s) in
// 2 allocation(s) and Cell.n = 2 (elements never disposed).
// =====================================================================
BEZEL_TEST(codegen_owned_polymorphic_list_iface_field_element_leak_2608,
           ireadonlylist_field_owning_backing_elements_disposed_spec_28_11) {
    JitResult r = runAotChecked(kOwningBackedElements);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);          // LSan-clean (the #2608 leak flips this)
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0 — disposed exactly once
}

// =====================================================================
// #2608 (B) — BORROWING-BACKING UAF GUARD: the same static field type with a
// borrowing Array<Cell> backing must NOT dispose its elements. This is the
// case that makes a `collIsOwning` head-name widening unsound.
// =====================================================================
BEZEL_TEST(codegen_owned_polymorphic_list_iface_field_element_leak_2608,
           ireadonlylist_field_borrowing_backing_elements_not_disposed_spec_4_3) {
    JitResult r = runAotChecked(kBorrowingBackedElements);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0 — no double-free
}

// =====================================================================
// #2608 (C) — REASSIGNMENT / EXACTLY-ONCE through the drop-on-overwrite path.
// =====================================================================
BEZEL_TEST(codegen_owned_polymorphic_list_iface_field_element_leak_2608,
           ireadonlylist_field_owning_backing_reassign_exactly_once_spec_28_11) {
    JitResult r = runAotChecked(kOwningBackedReassign);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0 — 3 created, 3 disposed
}

// =====================================================================
// #2608 (D) — CAPTURED PRIMARY-CTOR-PARAM (§22.10): the callee sees only the
// erased interface, so no static analysis could classify this shape.
// =====================================================================
BEZEL_TEST(codegen_owned_polymorphic_list_iface_field_element_leak_2608,
           ireadonlylist_ctor_param_owning_backing_disposed_spec_22_10_28_11) {
    JitResult r = runAotChecked(kOwningBackedCtorParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0
}

// =====================================================================
// #2608 (E) — BORROWING REASSIGNMENT UAF GUARD: a displaced borrowing backing
// must free its storage without disposing the still-live borrowed element.
// =====================================================================
BEZEL_TEST(codegen_owned_polymorphic_list_iface_field_element_leak_2608,
           ireadonlylist_field_borrowing_backing_reassign_no_uaf_spec_4_3) {
    JitResult r = runAotChecked(kBorrowingBackedReassign);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0
}

// =====================================================================
// #2608 (F) — owned IList<string> CONTROL (#2603): the static string-element
// release thunk still fires; the dynamic path must not divert it.
// =====================================================================
BEZEL_TEST(codegen_owned_polymorphic_list_iface_field_element_leak_2608,
           owned_ilist_string_field_static_release_preserved_spec_28_11) {
    JitResult r = runAotChecked(kOwnedIListStringControl);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);   // 300 mod 256
}

// =====================================================================
// #2608 (G) — owned IOwningList<Cell> CONTROL (#2606): the statically-owning
// interface keeps its static disposer and is disposed exactly once.
// =====================================================================
BEZEL_TEST(codegen_owned_polymorphic_list_iface_field_element_leak_2608,
           owned_iowninglist_field_static_owning_preserved_spec_28_11) {
    JitResult r = runAotChecked(kOwnedIOwningListControl);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0
}

// =====================================================================
// #2608 (H) — owned OwningArray<Cell> CONTROL: the concrete owning-array path
// is unaffected by construction-site tagging.
// =====================================================================
BEZEL_TEST(codegen_owned_polymorphic_list_iface_field_element_leak_2608,
           owned_owningarray_field_concrete_path_unaffected_spec_28_5) {
    JitResult r = runAotChecked(kOwnedOwningArrayControl);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0
}
