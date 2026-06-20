// fix #824 / §28.5 / §28.9: reassigning an `owned` collection field
// (OwnedArray<T> etc.) now disposes the prior collection and its owned
// elements before overwriting the pointer. Before the fix the old pointer was
// silently leaked — `Cell.n` never returned to 0 after a Replace() call.
//
// Tests use the Cell destructor-counter pattern (Cell.n incremented in ctor,
// decremented in dtor) so disposal is observable via the JIT exit code.
// rc=0 ↔ all Cells disposed exactly once; rc>0 ↔ leak; crash ↔ double-free.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// Boilerplate: Cell tracks live-instance count in Cell.n; Box holds an
// `owned OwnedArray<Cell>` field that starts with one element.
static constexpr const char* kPrefix =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

}  // namespace

// §28.5 / §28.9 / #824: single reassignment via a fresh OwnedArray local —
// the original OwnedArray<Cell> (and its one Cell) must be disposed on Replace().
BEZEL_TEST(owned_collection_field_reassign_824, single_reassign_rc0) {
    BEZEL_EXPECT_EQ(run(std::string(kPrefix) +
        "class Box "
        "  private owned _arr: OwnedArray<Cell>; "
        "  public constructor() "
        "    _arr := new OwnedArray<Cell>(); "
        "    _arr.Add(new Cell()); "
        "  end constructor; "
        "  public method Replace() "
        "    var fresh := new OwnedArray<Cell>(); "
        "    fresh.Add(new Cell()); "
        "    _arr := fresh; "
        "  end Replace; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); b.Replace(); end; "
        "  return Cell.n; "
        "end Main; end C;"), 0);
}

// §28.5 / §28.9 / #824: double reassignment — both the original and the first
// replacement OwnedArray<Cell> (each with one Cell) must be disposed before the
// second replacement is stored; after disposal of Box only the second
// replacement's Cell is disposed. All three Cells must be disposed: rc=0.
BEZEL_TEST(owned_collection_field_reassign_824, double_reassign_rc0) {
    BEZEL_EXPECT_EQ(run(std::string(kPrefix) +
        "class Box "
        "  private owned _arr: OwnedArray<Cell>; "
        "  public constructor() "
        "    _arr := new OwnedArray<Cell>(); "
        "    _arr.Add(new Cell()); "
        "  end constructor; "
        "  public method Replace1() "
        "    var f1 := new OwnedArray<Cell>(); "
        "    f1.Add(new Cell()); "
        "    _arr := f1; "
        "  end Replace1; "
        "  public method Replace2() "
        "    var f2 := new OwnedArray<Cell>(); "
        "    f2.Add(new Cell()); "
        "    _arr := f2; "
        "  end Replace2; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); b.Replace1(); b.Replace2(); end; "
        "  return Cell.n; "
        "end Main; end C;"), 0);
}

// §28.5 / §28.9 / #824: collection-literal reassignment `_arr := [new Cell()]`
// goes through an early-exit path (§15.4 #820); it must also dispose the old
// collection before overwriting.
BEZEL_TEST(owned_collection_field_reassign_824, literal_reassign_rc0) {
    BEZEL_EXPECT_EQ(run(std::string(kPrefix) +
        "class Box "
        "  private owned _arr: OwnedArray<Cell>; "
        "  public constructor() "
        "    _arr := new OwnedArray<Cell>(); "
        "    _arr.Add(new Cell()); "
        "  end constructor; "
        "  public method ReplaceWithLiteral() "
        "    _arr := [new Cell()]; "
        "  end ReplaceWithLiteral; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); b.ReplaceWithLiteral(); end; "
        "  return Cell.n; "
        "end Main; end C;"), 0);
}

// §28.5 / §28.9 / #824 (control / no-reassign): ctor-only path. The first
// assignment `_arr := new OwnedArray<Cell>()` in the constructor sets a
// null/uninitialized old slot; the null-safe bz_array_free must not crash.
// After Box is disposed, Cell.n must be 0.
BEZEL_TEST(owned_collection_field_reassign_824, ctor_only_no_crash_rc0) {
    BEZEL_EXPECT_EQ(run(std::string(kPrefix) +
        "class Box "
        "  private owned _arr: OwnedArray<Cell>; "
        "  public constructor() "
        "    _arr := new OwnedArray<Cell>(); "
        "    _arr.Add(new Cell()); "
        "  end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); end; "
        "  return Cell.n; "
        "end Main; end C;"), 0);
}

// §28.5 / §28.9 / #824: after reassign, the surviving collection's elements
// are still accessible; Box.GetFirst() must return the NEW cell's value (42),
// not the old one's (10). And after disposal Cell.n must be 0.
BEZEL_TEST(owned_collection_field_reassign_824, survivor_accessible_rc0) {
    // Cell carries a public val field so we can distinguish old vs new element.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell "
        "  public static n: int := 0; "
        "  public val: int; "
        "  public constructor(v: int) "
        "    Cell.n := Cell.n + 1; val := v; "
        "  end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class Box "
        "  private owned _arr: OwnedArray<Cell>; "
        "  public constructor() "
        "    _arr := new OwnedArray<Cell>(); "
        "    _arr.Add(new Cell(10)); "
        "  end constructor; "
        "  public method Replace() "
        "    var fresh := new OwnedArray<Cell>(); "
        "    fresh.Add(new Cell(42)); "
        "    _arr := fresh; "
        "  end Replace; "
        "  public method GetFirst(): int return _arr[0].val; end GetFirst; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do "
        "    using var b := new Box(); "
        "    b.Replace(); "
        "    var v := b.GetFirst(); "
        "    if v <> 42 then return 99; end; "
        "  end; "
        "  return Cell.n; "
        "end Main; end C;"), 0);
}

// §28.9 / #824 (no-double-free guard): verify Cell.n never goes BELOW 0.
// A double-free would decrement Cell.n past 0; we track the minimum observed
// value via a second static and encode it in the return code.
BEZEL_TEST(owned_collection_field_reassign_824, no_double_free) {
    BEZEL_EXPECT_EQ(run(
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
        "end Cell; "
        "class Box "
        "  private owned _arr: OwnedArray<Cell>; "
        "  public constructor() "
        "    _arr := new OwnedArray<Cell>(); "
        "    _arr.Add(new Cell()); "
        "    _arr.Add(new Cell()); "
        "  end constructor; "
        "  public method Replace() "
        "    var fresh := new OwnedArray<Cell>(); "
        "    fresh.Add(new Cell()); "
        "    _arr := fresh; "
        "  end Replace; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  do using var b := new Box(); b.Replace(); b.Replace(); end; "
        "  if Cell.n <> 0 then return Cell.n + 100; end; "
        "  if Cell.mn < 0 then return Cell.mn + 200; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}
