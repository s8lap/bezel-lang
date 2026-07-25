// fix #2185 / §28.8 / §28.8.1 / §28.7: `move _field` of an owning-collection-
// typed OWNED FIELD (OwningArray<T> / Array<T> / OwningDictionary<K,V> /
// LinkedList<T> — anything stored as a heap collection handle whose disposal
// is owned by the enclosing class) now nulls the source slot at the move, so
// the spec-canonical build-before-take / restore-then-return shape
// (`using var t := move _arr; _arr := fresh;`) is sound.
//
// Before the fix, the move codegen nulled the slot only for class/interface
// and iterator-interface owned fields (isOwnedReferenceField /
// isOwnedIteratorInterfaceField); owning-COLLECTION fields slipped through
// (ownedCollectionFieldType is the separate predicate that classifies them).
// The un-null'd field kept the moved-out bz_array_t*/bz_llist_t*/bz_dict_t*
// pointer, so the §28.7 drop-on-overwrite store on the subsequent reassignment
// (`_arr := fresh` → lowerOwnedCollectionFieldStore) called bz_array_free on
// the value the new owner (the using-var `t`) already owns — and `t`'s
// scope-end disposal then freed the same block again: free() on a bogus
// header SEGV / ASan DEALLOCATE-WRITE on address 0xfffffffffffffffb, in
// bz$Ledger$Rotate, on the moved-out binding's bz_array_free. The collection
// was empty in the original report — the corruption is in the moved handle
// itself, not in element disposal.
//
// Tests use the Cell destructor-counter pattern (Cell.n incremented in ctor,
// decremented in dtor); the `mn` static also tracks the minimum observed
// value so a double-decrement (mn < 0) is observable too. rc=0 ↔ all Cells
// disposed exactly once; crash ↔ the SEGV that this fix repairs.
//
// Note on scoping: each Main wraps the owned Box/Ledger in a `do using var
// b := ...; ... end;` block so the scope-end disposal of `b` (which disposes
// the owning-collection field — and with it, the surviving replacement) runs
// BEFORE `return Cell.n`. (Returning Cell.n before the `using var` scope ends
// would observe an apparent "leak" that is just the not-yet-run scope-end
// disposal — a test-authoring artifact, not a real leak. See the 824 test
// file for the same `do ... end; return Cell.n;` discipline.)

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// Boilerplate: Cell tracks live-instance count in Cell.n; the `mn` static also
// tracks the minimum so a double-decrement (double-free of the same Cell) is
// observable as mn<0, not just as a final n>0 leak.
static constexpr const char* kPrefix =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public static mn: int := 0; "
    "  public constructor() "
    "    Cell.n := Cell.n + 1; "
    "    if Cell.n < Cell.mn then Cell.mn := Cell.n; end; "
    "  end constructor; "
    "  destructor "
    "    Cell.n := Cell.n - 1; "
    "    if Cell.n < Cell.mn then Cell.mn := Cell.n; end; "
    "  end destructor; "
    "end Cell; ";

}  // namespace

// §28.8.1 / §28.7 / #2185 (bare form, EMPTY collection — the issue's exact
// repro): `move _arr` of an owning-collection owned FIELD where the collection
// holds no elements. Before the fix this SEGV'd inside the moved-out binding's
// bz_array_free on disposal (free() on a bogus header). The fix nulls the
// field slot at the move, so the §28.7 drop-on-overwrite on the subsequent
// `_arr := fresh` is a no-op (bz_array_free is null-safe) and the moved-out
// binding's scope-end disposal frees the collection exactly once.
BEZEL_TEST(owned_collection_field_move_2185, bare_empty_no_crash) {
    BEZEL_EXPECT_EQ(run(std::string(kPrefix) +
        "class Ledger "
        "  private owned _transactions: OwningArray<Cell>; "
        "  public constructor() "
        "    _transactions := new OwningArray<Cell>(); "
        "  end constructor; "
        "  public method Rotate() "
        "    var fresh := new OwningArray<Cell>(); "
        "    using var taken := move _transactions; "
        "    _transactions := fresh; "
        "  end Rotate; "
        "  destructor end destructor; "
        "end Ledger; "
        "class C public static method Main(): int "
        "  do using var l := new Ledger(); l.Rotate(); end; "
        "  if Cell.n <> 0 then return Cell.n + 100; end; "
        "  if Cell.mn < 0 then return Cell.mn + 200; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §28.8.1 / §28.11 / #2185 (bare form, with ELEMENTS): the issue text requires
// "the same program with elements present must dispose each element exactly
// once, §28.11 reverse order." Two Cells are added to _transactions in the
// ctor, a third is added to `fresh`; after Rotate the two original Cells are
// disposed via the moved-out `taken` and the third via the owner's later
// disposal of the field (now holding `fresh`). rc=0 ↔ exactly-once.
BEZEL_TEST(owned_collection_field_move_2185, bare_with_elements_rc0) {
    BEZEL_EXPECT_EQ(run(std::string(kPrefix) +
        "class Ledger "
        "  private owned _transactions: OwningArray<Cell>; "
        "  public constructor() "
        "    _transactions := new OwningArray<Cell>(); "
        "    _transactions.Add(new Cell()); "
        "    _transactions.Add(new Cell()); "
        "  end constructor; "
        "  public method Rotate() "
        "    var fresh := new OwningArray<Cell>(); "
        "    fresh.Add(new Cell()); "
        "    using var taken := move _transactions; "
        "    _transactions := fresh; "
        "  end Rotate; "
        "  destructor end destructor; "
        "end Ledger; "
        "class C public static method Main(): int "
        "  do using var l := new Ledger(); l.Rotate(); end; "
        "  if Cell.n <> 0 then return Cell.n + 100; end; "
        "  if Cell.mn < 0 then return Cell.mn + 200; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §28.8.1 / §28.7 / #2185 (QUALIFIED form, `move this._field` — form-parity
// with the bare form, #825/#897/#898): same defect, same fix. Before the fix
// the qualified form also kept the moved-out pointer in the slot and crashed
// the same way on the subsequent `this._arr := fresh`.
BEZEL_TEST(owned_collection_field_move_2185, qualified_empty_no_crash) {
    BEZEL_EXPECT_EQ(run(std::string(kPrefix) +
        "class Ledger "
        "  private owned _transactions: OwningArray<Cell>; "
        "  public constructor() "
        "    _transactions := new OwningArray<Cell>(); "
        "  end constructor; "
        "  public method Rotate() "
        "    var fresh := new OwningArray<Cell>(); "
        "    using var taken := move this._transactions; "
        "    this._transactions := fresh; "
        "  end Rotate; "
        "  destructor end destructor; "
        "end Ledger; "
        "class C public static method Main(): int "
        "  do using var l := new Ledger(); l.Rotate(); end; "
        "  if Cell.n <> 0 then return Cell.n + 100; end; "
        "  if Cell.mn < 0 then return Cell.mn + 200; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §28.8.1 / §28.11 / #2185 (QUALIFIED form, with elements): mirror of
// bare_with_elements_rc0 through the `move this._field` spelling.
BEZEL_TEST(owned_collection_field_move_2185, qualified_with_elements_rc0) {
    BEZEL_EXPECT_EQ(run(std::string(kPrefix) +
        "class Ledger "
        "  private owned _transactions: OwningArray<Cell>; "
        "  public constructor() "
        "    _transactions := new OwningArray<Cell>(); "
        "    _transactions.Add(new Cell()); "
        "    _transactions.Add(new Cell()); "
        "  end constructor; "
        "  public method Rotate() "
        "    var fresh := new OwningArray<Cell>(); "
        "    fresh.Add(new Cell()); "
        "    using var taken := move this._transactions; "
        "    this._transactions := fresh; "
        "  end Rotate; "
        "  destructor end destructor; "
        "end Ledger; "
        "class C public static method Main(): int "
        "  do using var l := new Ledger(); l.Rotate(); end; "
        "  if Cell.n <> 0 then return Cell.n + 100; end; "
        "  if Cell.mn < 0 then return Cell.mn + 200; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §28.8 / §28.8.1 / #2185 (owned-RETURN take idiom — the #1814 corrected
// TakeTransactions shape drafted for §28.6): the moved-out collection is
// RETURNED through an `owned OwningArray<T>` slot and the caller binds it to
// a `using var`. The build-before-take / restore-then-return form must
// transfer the elements to the caller and dispose the replacement via the
// owner's later scope-end. rc=0 ↔ exactly-once across both disposal sites.
BEZEL_TEST(owned_collection_field_move_2185, owned_return_take_idiom_rc0) {
    BEZEL_EXPECT_EQ(run(std::string(kPrefix) +
        "class Ledger "
        "  private owned _transactions: OwningArray<Cell>; "
        "  public constructor() "
        "    _transactions := new OwningArray<Cell>(); "
        "    _transactions.Add(new Cell()); "
        "    _transactions.Add(new Cell()); "
        "  end constructor; "
        "  public method TakeTransactions(): owned OwningArray<Cell> "
        "    var fresh := new OwningArray<Cell>(); "
        "    fresh.Add(new Cell()); "
        "    var taken := move _transactions; "
        "    _transactions := fresh; "
        "    return taken; "
        "  end TakeTransactions; "
        "  destructor end destructor; "
        "end Ledger; "
        "class C public static method Main(): int "
        "  do "
        "    using var l := new Ledger(); "
        "    using var t := l.TakeTransactions(); "
        "    if t.Count <> 2 then return 99; end; "
        "  end; "
        "  if Cell.n <> 0 then return Cell.n + 100; end; "
        "  if Cell.mn < 0 then return Cell.mn + 200; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §28.9 / #2185 (no-double-free guard, mirroring the 824 pattern): exercise
// the move+restore shape TWICE in succession on the same field — the second
// Rotate moves out the replacement (from the first Rotate) and stores a third
// fresh collection. A regression that re-introduced the un-null'd slot would
// either crash (double-free SEGV on the second Rotate) or drive Cell.n below
// 0 (mn<0). The mn sentinel catches a double-decrement even when the final
// Cell.n happens to land at 0 by coincidence.
BEZEL_TEST(owned_collection_field_move_2185, double_rotate_no_double_free) {
    BEZEL_EXPECT_EQ(run(std::string(kPrefix) +
        "class Ledger "
        "  private owned _transactions: OwningArray<Cell>; "
        "  public constructor() "
        "    _transactions := new OwningArray<Cell>(); "
        "    _transactions.Add(new Cell()); "
        "    _transactions.Add(new Cell()); "
        "  end constructor; "
        "  public method Rotate() "
        "    var fresh := new OwningArray<Cell>(); "
        "    fresh.Add(new Cell()); "
        "    fresh.Add(new Cell()); "
        "    using var taken := move _transactions; "
        "    _transactions := fresh; "
        "  end Rotate; "
        "  destructor end destructor; "
        "end Ledger; "
        "class C public static method Main(): int "
        "  do using var l := new Ledger(); l.Rotate(); l.Rotate(); end; "
        "  if Cell.n <> 0 then return Cell.n + 100; end; "
        "  if Cell.mn < 0 then return Cell.mn + 200; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}
