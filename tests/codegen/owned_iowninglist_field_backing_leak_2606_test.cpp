// fix #2606 (codegen leak, §28.5 / §28.9 / §28.11 / stdlib §4.5, v0.236): an
// `owned`-modifier field whose STATIC type is `IOwningList<T>` (the OWNING list
// interface) leaks its `bz_array_t*` backing storage at scope end, and — when
// populated — its owned class elements too. This is the OWNING-variant sibling
// of #2603 (which fixed the BORROWING list interfaces IList<T> /
// IReadOnlyList<T>).
//
// ROOT CAUSE: `Emitter::ownedCollectionFieldType` (LlvmBackend_impl.hpp ~6427)
// admitted the borrowing list interfaces via `isBorrowingListInterfaceName`
// (#2603) but EXCLUDED `IOwningList<T>`, so an `owned IOwningList<T>` field
// returned nullptr and never reached `emitOwnedFieldDisposals`'s collection
// path — its backing (from `bz_array_alloc`) was never freed. Admitting it
// alone was insufficient and deliberately deferred: the element-ownership flag
// `collIsOwning` is derived via `common::isOwningPrefixedName(headName())`
// (LlvmBackend_Disposal.cpp:602 / LlvmBackend_Setup.cpp:15595), and
// `isOwningPrefixedName("IOwningList")` is FALSE (the prefix is `"Owning"` but
// the head starts with `"I"`). So admitting IOwningList through the disposal
// path with the borrowing classification would free its backing but build NO
// element disposer — its owned class elements would leak.
//
// FIX (two coordinated changes):
//   1. Add `isOwningListInterfaceName` (isListInterfaceName && owning) next to
//      `isBorrowingListInterfaceName`, and admit it in `ownedCollectionFieldType`
//      (both the captured-primary-ctor-param arm and the FieldDecl arm). The
//      field then routes through `emitOwnedFieldDisposals` /
//      `lowerOwnedCollectionFieldStore`.
//   2. Mark the family OWNING at the two `collIsOwning` derivation sites
//      (Disposal.cpp:602 / Setup.cpp:15595) by OR-ing in
//      `isOwningListInterfaceName(collTy)` — so `elementPolicyFor(elem,
//      owning=true)` builds the class-element disposer thunk.
//
// This makes `owned IOwningList<T>` behave IDENTICALLY to
// `owned OwningArray<T>` (the only implementer of IOwningList per stdlib §4.5):
// `bz_array_free` frees the array backing (IOwningList is array-backed per
// issue #401, same bz_array_t* as OwningArray<T>), and the element disposer
// disposes each owned class element (stdlib §4.5: "Disposing an OwningArray<T>
// automatically disposes every element"). String elements are rejected at
// instantiation for owning collections (stdlib §4.4), so the borrowing-family
// string-element-release gate is irrelevant here.
//
// OWNING-SEMANTICS VERIFICATION: IOwningList<T> is the owning-set, disposing
// variant (Calls.cpp `isListInterfaceName`: outOwning=true ONLY for IOwningList;
// stdlib §4.5 names it the owning list interface whose elements are owned).
// Classifying it OWNING (collIsOwning=true) is therefore correct: its elements
// ARE owned by the holder. The inverse misclassification (borrowing) is the
// pre-fix leak; the test's live-object counter distinguishes the two.
//
// OBSERVABILITY: each Cell counts net-live instances (ctor ++, dtor --); Main
// returns Cell.n, so the exit code is the number of Cells still alive. 0 ==
// every Cell disposed exactly once (no leak, no double-free). runAotChecked's
// LSan additionally flags the `bz_array_t*` backing leak. Pre-fix the cardinal
// leaked 16800 byte(s) / 200 allocations (100 empty backings); the
// element-disposal case leaked the backing AND kept Cell.n > 0.
//
// REGRESSION COVERAGE:
//   * (A) CARDINAL — owned IOwningList<Cell> field, the issue's repro (100x
//     Holder, empty backing only — `.Add` through IOwningList is rejected per
//     #853, so the field cannot be populated THROUGH THE INTERFACE). Pre-fix
//     LSan: 16800 byte(s) / 200 allocations. Post-fix: LSan-clean, exit 100.
//   * (B) ELEMENT DISPOSAL — owned IOwningList<Cell> field POPULATED via a
//     concrete OwningArray<Cell> producer assigned to the interface field
//     (`.Add` on the CONCRETE type is supported; the upcast store routes
//     through lowerOwnedCollectionFieldStore). Proves collIsOwning=TRUE builds
//     the class-element disposer: Cell.n returns to 0 after the holder is
//     disposed. Pre-fix: Cell.n = 2 (cells never disposed) + backing leak.
//     Post-fix: Cell.n = 0, LSan-clean.
//   * (C) REASSIGNMENT / EXACTLY-ONCE — owned IOwningList<Cell> field
//     REASSIGNED twice; the displaced backings are freed and their owned
//     elements disposed EXACTLY ONCE via the disposer (lowerOwnedCollection
//     FieldStore, Setup.cpp:15595). Cell.n returns to 0 (never negative — a
//     double-free would crash under ASan / drive the counter negative).
//     Post-fix: LSan-clean, exit 0.
//   * (D) owned OwningArray<Cell> FIELD CONTROL — the concrete owning array
//     (collIsOwning via isOwningPrefixedName("OwningArray")=true). Must stay
//     clean (no regression in the concrete owning path). exit 0.
//   * (E) owned IList<string> FIELD CONTROL (#2603) — the BORROWING interface
//     sibling. The collIsOwning widening must NOT flip it owning (would
//     double-free its borrowing string elements). LSan-clean, exit 44.
//
// SPEC CITATION: §28.5 (owned fields — disposal cascades into the holder's
// Dispose), §28.9 (runtime-managed types), §28.11 (collection element ownership
// — owning family owns its elements), stdlib §4.5 (OwningArray<T> /
// IOwningList<T> — owning list interface, disposing an array disposes every
// element), §4.1 / issue #401 (the list interfaces are array-backed, same
// bz_array_t* as Array<T>).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ASan toolchain skip-guard (matches the #2603 / #2598 test pattern).
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// ----------------------------------------------------------------------
// Probe A — CARDINAL: owned IOwningList<Cell> field backing-storage leak (the
// issue's repro). 100 iterations; each constructs a Holder whose `owned _arr:
// IOwningList<Cell>` field is allocated via bz_array_alloc (empty OwningArray)
// and dropped at scope end. `.Add` through IOwningList is rejected (#853), so
// only the empty backing leaks. Pre-fix LSan: 16800 byte(s) / 200 allocations.
// Post-fix: LSan-clean, exit 100 (sink 100 mod 256).
// ----------------------------------------------------------------------
const char* kOwnedIOwningListCellField =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _arr: IOwningList<Cell> := new OwningArray<Cell>(); "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    var i := 0; var sink := 0; "
    "    while i < 100 do "
    "      using var h := new Holder(); "
    "      sink := sink + 1; "
    "      i := i + 1; "
    "    end; "
    "    return sink mod 256; "  // 100
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe B — ELEMENT DISPOSAL: owned IOwningList<Cell> field POPULATED via a
// concrete OwningArray<Cell> producer assigned to the interface field. Proves
// collIsOwning=TRUE builds the class-element disposer: the 2 cells are
// disposed when the holder is disposed, so Cell.n returns to 0. Pre-fix:
// Cell.n = 2 (cells leak) + backing leak. Post-fix: Cell.n = 0, LSan-clean.
// The holder lives in Run(); Main reads Cell.n AFTER Run returns (holder
// disposed), so 0 is load-bearing for element disposal.
// ----------------------------------------------------------------------
const char* kOwnedIOwningListElementDisposal =
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
    "    var sink := C.Run(); "  // h disposed here -> cells disposed
    "    return Cell.n mod 256; "  // 0 if elements disposed (collIsOwning=true)
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe C — REASSIGNMENT / EXACTLY-ONCE: owned IOwningList<Cell> field
// reassigned twice. The displaced backings are freed and their owned elements
// disposed EXACTLY ONCE via the element disposer
// (lowerOwnedCollectionFieldStore, Setup.cpp:15595). Cell.n returns to 0
// (never negative — a double-free would crash under ASan). Post-fix:
// LSan-clean, exit 0.
// ----------------------------------------------------------------------
const char* kOwnedIOwningListReassign =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _arr: IOwningList<Cell> := new OwningArray<Cell>(); "
    "  public method FillAndReset(): int "
    "    var tmp := new OwningArray<Cell>(); "
    "    tmp.Add(new Cell()); "
    "    tmp.Add(new Cell()); "
    "    _arr := tmp; "            // dispose old empty; store tmp (2 cells)
    "    var tmp2 := new OwningArray<Cell>(); "
    "    tmp2.Add(new Cell()); "
    "    _arr := tmp2; "           // dispose tmp (2 cells via disposer); store tmp2
    "    return _arr.Count; "      // 1
    "  end FillAndReset; "
    "end Holder; "
    "class C "
    "  public static method Run(): int "
    "    using var h := new Holder(); "
    "    return h.FillAndReset(); "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "    // h disposed -> last cell disposed
    "    return Cell.n mod 256; "  // 0 (3 created, 3 disposed exactly once)
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// Probe D — owned OwningArray<Cell> FIELD CONTROL: the concrete owning array
// (collIsOwning via isOwningPrefixedName("OwningArray")=true). The widening
// must NOT regress the existing concrete owning path. LSan-clean, exit 0.
// ----------------------------------------------------------------------
const char* kOwnedOwningArrayCellField =
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

// ----------------------------------------------------------------------
// Probe E — owned IList<string> FIELD CONTROL (#2603): the BORROWING interface
// sibling. The collIsOwning widening must NOT flip IList owning (would
// double-free its borrowing string elements). Pre/post-fix LSan-clean,
// exit 44 (300 mod 256).
// ----------------------------------------------------------------------
const char* kOwnedIListStringField =
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
// Probe F — CAPTURED PRIMARY-CTOR-PARAM: `class Holder(owned v: IOwningList
// <Cell>)` captures the `owned` param as a synthesized field (§22.10) — the
// `posParam` arm of ownedCollectionFieldType (a DISTINCT code path from the
// FieldDecl arm). The param is populated via a concrete OwningArray<Cell>
// producer; when the holder is disposed the captured field's backing + owned
// elements are freed (Cell.n returns to 0). Post-fix: LSan-clean, exit 0.
// ----------------------------------------------------------------------
const char* kOwnedIOwningListCtorParam =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder(owned v: IOwningList<Cell>) "
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

}  // namespace

// =====================================================================
// #2606 — CARDINAL: owned IOwningList<Cell> field backing leak. Pre-fix LSan
// flags 16800 byte(s) / 200 allocations (100 empty backings). Post-fix the
// field's bz_array_t* backing is freed via bz_array_free at the holder's
// Dispose. LSan-clean + value 100.
// =====================================================================
BEZEL_TEST(codegen_owned_iowninglist_field_backing_leak_2606,
           owned_iowninglist_cell_field_backing_freed_spec_28_5_28_11) {
    JitResult r = runAotChecked(kOwnedIOwningListCellField);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);           // LSan-clean (the #2606 leak flips this)
    BEZEL_EXPECT_EQ(r.exitCode, 100);   // 100 mod 256 — independent value check
}

// =====================================================================
// #2606 — ELEMENT DISPOSAL: the KEY owning-classification test. The
// IOwningList<Cell> field is populated via a concrete OwningArray<Cell>
// producer; collIsOwning=TRUE builds the class-element disposer, so the cells
// are disposed when the holder is disposed (Cell.n returns to 0). Pre-fix:
// Cell.n = 2 (cells leak) + backing leak. Post-fix: Cell.n = 0, LSan-clean.
// =====================================================================
BEZEL_TEST(codegen_owned_iowninglist_field_backing_leak_2606,
           owned_iowninglist_cell_field_elements_disposed_spec_28_11) {
    JitResult r = runAotChecked(kOwnedIOwningListElementDisposal);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);     // Cell.n = 0 — elements disposed
}

// =====================================================================
// #2606 — REASSIGNMENT / EXACTLY-ONCE: the field is reassigned twice; the
// displaced backings are freed and their owned elements disposed EXACTLY ONCE
// (lowerOwnedCollectionFieldStore). Cell.n returns to 0 (never negative — a
// double-free would crash under ASan). LSan-clean + value 0.
// =====================================================================
BEZEL_TEST(codegen_owned_iowninglist_field_backing_leak_2606,
           owned_iowninglist_cell_field_reassign_exactly_once_spec_28_11) {
    JitResult r = runAotChecked(kOwnedIOwningListReassign);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);     // Cell.n = 0 — exactly-once, no double-free
}

// =====================================================================
// #2606 — owned OwningArray<Cell> FIELD regression guard: the collIsOwning
// widening must not regress the existing concrete owning-array field-disposal
// path. LSan-clean + value 0.
// =====================================================================
BEZEL_TEST(codegen_owned_iowninglist_field_backing_leak_2606,
           owned_owningarray_cell_field_remains_clean_spec_28_5_28_11) {
    JitResult r = runAotChecked(kOwnedOwningArrayCellField);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);     // Cell.n = 0
}

// =====================================================================
// #2606 — owned IList<string> FIELD regression guard (#2603): the collIsOwning
// widening must NOT flip the borrowing IList owning (would double-free its
// borrowing string elements). LSan-clean + value 44.
// =====================================================================
BEZEL_TEST(codegen_owned_iowninglist_field_backing_leak_2606,
           owned_ilist_string_field_remains_borrowing_spec_28_11) {
    JitResult r = runAotChecked(kOwnedIListStringField);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);    // 300 mod 256
}

// =====================================================================
// #2606 — CAPTURED PRIMARY-CTOR-PARAM: `class Holder(owned v: IOwningList
// <Cell>)` — the posParam arm of ownedCollectionFieldType (a DISTINCT code
// path from the FieldDecl arm, §22.10). The captured field's backing + owned
// elements are freed when the holder is disposed (Cell.n returns to 0).
// LSan-clean + value 0.
// =====================================================================
BEZEL_TEST(codegen_owned_iowninglist_field_backing_leak_2606,
           owned_iowninglist_ctor_param_field_disposed_spec_22_10_28_11) {
    JitResult r = runAotChecked(kOwnedIOwningListCtorParam);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);     // Cell.n = 0
}
