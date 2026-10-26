// fix #2635 (codegen leak, §28.5 / §28.11 / §15.4 / stdlib §4.3 / §4.5,
// v0.236): a COLLECTION-LITERAL-constructed owning array was never
// element-disposer-stamped, so it still leaked its elements when held in an
// `owned` field typed as a BORROWING read-only list interface.
//
// RESIDUAL OF #2608. #2608 made an owning collection record its element
// disposer in the `bz_array_t` header at construction
// (`bz_array_set_elem_disposer`), so the erased free site can recover it
// (`bz_array_free_dynamic`) — element ownership is a property of the DYNAMIC
// backing, which no static head name can recover, because per stdlib §4.3 "The
// two hierarchies share `IEnumerable<T>`, `IReadOnlyCollection<T>`, and
// `IReadOnlyList<T>` as common ancestors" and one such field type legitimately
// carries EITHER backing. But #2608 applied the stamp as a function-local
// lambda inside `lowerNew`'s `bz_array_alloc` arm, and TWO other construction
// sites call `bz_array_alloc` directly:
//   * `materializeHeapCollectionLiteral` (LlvmBackend_Setup.cpp) — the
//     `OwningArray`/`OwningStack`/`OwningQueue` literal;
//   * `materializeArrayBackedSetLiteral` (LlvmBackend_Setup.cpp) — `OwningSet`.
// Both produced UNTAGGED owning arrays, so the erased field freed them with a
// NULL element disposer exactly as before #2608.
//
// PROVEN MECHANICALLY ON 875a9ad BEFORE CODING: the (A) program below leaked
// `32 byte(s) in 2 allocation(s)` under LSan — the two `Cell`s — both frames in
// the literal's enclosing method, with the `bz_array_t` backing itself
// correctly freed. That is precisely the residual #2635 describes.
//
// NOTE ON THE ISSUE'S OWN REPRO — it does NOT compile, and these tests
// deliberately do not use it. #2635 proposes:
//     var tmp: OwningArray<Cell> := [new Cell()];
//     _f := tmp;                  // owned _f: IReadOnlyList<Cell>
// The store is rejected: "a borrow cannot be stored into an `owned` field …
// (§28.3 / §28.5 / §28.8)". A literal-initialised local is not an owned
// PRODUCTION for the purposes of that store, while the otherwise-identical
// `var tmp: OwningArray<Cell> := new OwningArray<Cell>();` is accepted (and was
// already correct via #2608). The defect is nevertheless real and reachable —
// through a method whose return type is `owned OwningArray<Cell>` and whose
// body returns the literal, which is the shape used throughout below.
//
// FIX: the stamp moved out of #2608's lambda into a shared Emitter member
// `stampOwningElemDisposer` (LlvmBackend_impl.hpp) that pairs "allocate an
// owning bz_array_t" with "tag it", and is now called from all three
// construction sites. Pairing them in one callable is what stops the next
// construction site from silently skipping the tag — the drift between the
// sites was the whole defect.
//
// WHY THIS CANNOT DOUBLE-FREE: `owning` is the caller's CONCRETE head-name
// test, so a BORROWING literal is never tagged and its header disposer stays
// NULL, which is bit-for-bit today's behaviour. The change is strictly monotone
// (NULL -> possibly-a-disposer) and only for a backing a concrete owning
// literal built. Test (B) is the guard and was verified to stay clean.
//
// OBSERVABILITY: each `Cell` counts net-live instances (ctor ++, dtor --) and
// Main returns `Cell.n`, so the exit code is the number of Cells still ALIVE.
// 0 == every Cell disposed EXACTLY ONCE — a leak drives it positive, a
// double-free drives it negative AND trips ASan. runAotChecked's LSan
// additionally flags the raw allocation leak, so neither failure mode can pass.
//
// SPEC CITATION: §28.5 (Owned fields), §28.11 (Collection element ownership),
// §15.4 (Collection literals), stdlib §4.3 (how the borrowing and owning
// hierarchies relate), stdlib §4.5 (`OwningArray<T>`).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// ----------------------------------------------------------------------
// (A) CARDINAL: an owned IReadOnlyList<Cell> field whose backing is an
// OwningArray<Cell> built by a COLLECTION LITERAL (§15.4) rather than by
// `new`. Pre-fix: 32 byte(s) leaked in 2 allocation(s), Cell.n = 2.
// ----------------------------------------------------------------------
const char* kOwningLiteralBackedElements =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _arr: IReadOnlyList<Cell> := new OwningArray<Cell>(); "
    "  private static method Make(): owned OwningArray<Cell> "
    "    return [new Cell(), new Cell()]; "   // §15.4 literal -> the uncovered site
    "  end Make; "
    "  public method Fill(): int "
    "    var tmp := Holder.Make(); "
    "    _arr := tmp; "                       // upcast store into the erased field
    "    return _arr.Count; "                 // 2
    "  end Fill; "
    "end Holder; "
    "class C "
    "  public static method Run(): int "
    "    using var h := new Holder(); "
    "    return h.Fill(); "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "               // h disposed -> cells disposed
    "    return Cell.n mod 256; "             // 0 iff disposed exactly once
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (B) BORROWING-LITERAL UAF GUARD — the polarity this fix must not break.
// The SAME erased field type, but the literal builds a BORROWING Array<Cell>
// (owned as storage, elements borrowed) over a Cell owned by an enclosing
// `using var`. The element must NOT be disposed by the field. Stamping this
// backing would be a double-free. Correct pre-fix and post-fix.
// ----------------------------------------------------------------------
const char* kBorrowingLiteralBackedElements =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _arr: IReadOnlyList<Cell> := new Array<Cell>(); "
    "  private static method Make(c: Cell): owned Array<Cell> "
    "    return [c]; "                        // borrowing literal — never tagged
    "  end Make; "
    "  public method Fill(c: Cell): int "
    "    var tmp := Holder.Make(c); "
    "    _arr := tmp; "
    "    return _arr.Count; "                 // 1
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
    "    return Cell.n mod 256; "             // 0 — c disposed once by its own
    "  end Main; "                            //     `using var`, not by the field
    "end C;";

// ----------------------------------------------------------------------
// (C) REASSIGNMENT / EXACTLY-ONCE: the erased field is reassigned, so the
// displaced literal-built backing goes through the drop-on-overwrite path
// (lowerOwnedCollectionFieldStore, the second #2608 free site). 3 Cells
// created, 3 disposed exactly once — a double-free would drive Cell.n
// negative and trip ASan, a leak would drive it positive.
// ----------------------------------------------------------------------
const char* kOwningLiteralReassign =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _arr: IReadOnlyList<Cell> := new OwningArray<Cell>(); "
    "  private static method One(): owned OwningArray<Cell> "
    "    return [new Cell()]; "
    "  end One; "
    "  private static method Two(): owned OwningArray<Cell> "
    "    return [new Cell(), new Cell()]; "
    "  end Two; "
    "  public method Fill(): int "
    "    var a := Holder.One(); "
    "    _arr := a; "                         // first backing (1 Cell)
    "    var b := Holder.Two(); "
    "    _arr := b; "                         // displaces it -> drop-on-overwrite
    "    return _arr.Count; "                 // 2
    "  end Fill; "
    "end Holder; "
    "class C "
    "  public static method Run(): int "
    "    using var h := new Holder(); "
    "    return h.Fill(); "
    "  end Run; "
    "  public static method Main(): int "
    "    var sink := C.Run(); "
    "    return Cell.n mod 256; "             // 0 — all 3 disposed exactly once
    "  end Main; "
    "end C;";

// ----------------------------------------------------------------------
// (D) CONCRETE-FIELD CONTROL: the same literal-built OwningArray<Cell> stored
// in a field typed CONCRETELY. This shape takes the STATIC disposer path (the
// field's own head name is owning), so it was correct before the fix and must
// remain correct after — the guard that the new stamp does not disturb a site
// that already derives its disposer statically.
// ----------------------------------------------------------------------
const char* kConcreteOwningArrayLiteralControl =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _arr: OwningArray<Cell> := new OwningArray<Cell>(); "
    "  private static method Make(): owned OwningArray<Cell> "
    "    return [new Cell(), new Cell()]; "
    "  end Make; "
    "  public method Fill(): int "
    "    var tmp := Holder.Make(); "
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
    "    return Cell.n mod 256; "             // 0
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2635 (A) — CARDINAL: literal-constructed OWNING backing behind an erased
// read-only list interface. Leaked 2 Cells before the fix.
// =====================================================================
BEZEL_TEST(codegen_owning_literal_elem_disposer_2635,
           ireadonlylist_field_owning_literal_backing_elements_disposed_spec_28_11) {
    JitResult r = runAotChecked(kOwningLiteralBackedElements);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0 — disposed exactly once
}

// =====================================================================
// #2635 (B) — BORROWING-LITERAL UAF GUARD (stdlib §4.3): the same field type
// with a borrowing literal backing must NOT dispose its elements.
// =====================================================================
BEZEL_TEST(codegen_owning_literal_elem_disposer_2635,
           ireadonlylist_field_borrowing_literal_backing_not_disposed_spec_4_3) {
    JitResult r = runAotChecked(kBorrowingLiteralBackedElements);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0 — no double-free
}

// =====================================================================
// #2635 (C) — REASSIGNMENT / EXACTLY-ONCE through the drop-on-overwrite path
// with literal-constructed backings.
// =====================================================================
BEZEL_TEST(codegen_owning_literal_elem_disposer_2635,
           ireadonlylist_field_owning_literal_reassign_exactly_once_spec_28_11) {
    JitResult r = runAotChecked(kOwningLiteralReassign);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0 — 3 created, 3 disposed
}

// =====================================================================
// #2635 (D) — CONCRETE OwningArray<Cell> FIELD CONTROL: the static disposer
// path is unaffected by the new construction-site stamp.
// =====================================================================
BEZEL_TEST(codegen_owning_literal_elem_disposer_2635,
           owningarray_field_literal_backing_static_path_unaffected_spec_28_5) {
    JitResult r = runAotChecked(kConcreteOwningArrayLiteralControl);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);    // Cell.n = 0
}
