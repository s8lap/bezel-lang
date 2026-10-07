// Regression test for #5459: lowerOwnedCollectionFieldStore has no identity guard,
// causing a self-assignment to an owning collection field to incorrectly dispose
// the value.
//
// The fix adds an identity guard: doDispose = (oldVal != null) && (oldVal != newVal).
// When oldVal == newVal (same pointer), disposal is skipped.
//
// IMPORTANT: Bezel's type system rejects `this._items := this._items` because
// reading an `owned` field returns a BORROW, and storing a borrow into an `owned`
// field is a type error ("a borrow cannot be stored into an owned field").
// This means the exact self-assignment shown in the issue CANNOT compile.
// The identity guard is a DEFENSIVE measure: it prevents incorrect disposal if
// ANY code path produces oldVal == newVal (whether through self-assignment,
// aliased parameters, or other mechanisms).
//
// The positive-direction row proves that assigning a DIFFERENT collection still
// disposes correctly (without this row, a fix that skips ALL disposal would pass
// the self-assignment test).
//
// Spec: §28.5 / §28.9 / §28.11 (v0.315)

#include <string>
#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A double-free / use-after-free flips r.ran to false -> -999, distinct
// from any legal exit value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// §28.5 / §28.9 / §28.11 (#5459): assigning a DIFFERENT collection to an
// owning collection field must dispose the displaced collection. This row proves
// the disposal path is NOT broken by the identity guard. Without this row,
// a fix that skips ALL disposal would pass the self-assignment test.
BEZEL_TEST(codegen_owned_collection_self_assign_5459,
           different_collection_still_disposes) {
    std::string src =
        "module M; "
        "class Cell "
        "  public static n: int := 0; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class Box "
        "  public owned _items: OwningArray<Cell>; "
        "  public constructor() "
        "    _items := new OwningArray<Cell>(); "
        "    _items.Add(new Cell()); "
        "  end constructor; "
        "  public method Replace() "
        "    var fresh := new OwningArray<Cell>(); "
        "    fresh.Add(new Cell()); "
        "    _items := fresh; "
        "  end Replace; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  for var i in 0..500 do "
        "    using var b := new Box(); "
        "    b.Replace(); "
        "  end for; "
        "  return Cell.n; "
        "end Main; end C;";
    // 501 iterations × 2 cells = 1002 created. Replace disposes 1, destructor 1.
    BEZEL_EXPECT_EQ(run(src), 0);
}

// OwningDictionary variant: different collection assignment still disposes.
BEZEL_TEST(codegen_owned_collection_self_assign_5459,
           dict_different_collection_still_disposes) {
    std::string src =
        "module M; "
        "class Cell "
        "  public static n: int := 0; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class Box "
        "  public owned _dict: OwningDictionary<string, Cell>; "
        "  public constructor() "
        "    _dict := new OwningDictionary<string, Cell>(); "
        "    _dict.Add(\"x\", new Cell()); "
        "  end constructor; "
        "  public method Replace() "
        "    var fresh := new OwningDictionary<string, Cell>(); "
        "    fresh.Add(\"y\", new Cell()); "
        "    _dict := fresh; "
        "  end Replace; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  for var i in 0..500 do "
        "    using var b := new Box(); "
        "    b.Replace(); "
        "  end for; "
        "  return Cell.n; "
        "end Main; end C;";
    // 501 iterations × 2 cells = 1002 created. Replace disposes 1, destructor 1.
    BEZEL_EXPECT_EQ(run(src), 0);
}

// Note: `this._items := this._items` is rejected by the type checker as
// "a borrow cannot be stored into an owned field" — this is CORRECT behavior.
// The identity guard is a defensive measure that ensures correct behavior if any
// code path produces oldVal == newVal at the LLVM IR level.

}  // namespace