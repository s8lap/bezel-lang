// Codegen — issue #919 (discovery sweep #7): `OwnedArray<class T>.Set(i, v)`
// overwrote slot i but did NOT dispose the displaced (owned) old element — a
// leak. The `.Set` handler used the disposing setter only for a borrowing
// Array<string> (releasing the old string); an owning class-element array fell
// to the plain bz_array_set. The array owns the replaced element, so disposing
// it (exactly as the indexer setter `arr[i] := v` already does) is sound — no
// double-free. §28.11 / §4.5.
//
// NOTE: `Set` is not declared on OwnedArray in stdlib §4.5 (the indexer is the
// spec surface), so its acceptance is a separate BAD_ACCEPT; this test pins the
// leak fix (the actual harm) and that the replacement value is correct.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kCell =
    "class Cell public static n: int := 0; public id: int; "
    "  public constructor(i: int) Cell.n:=Cell.n+1; id:=i; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// The replaced element is disposed (no leak): two Adds + one Set, all disposed.
BEZEL_TEST(owned_array_set_dispose_919, set_disposes_displaced_element) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    using var arr := new OwnedArray<Cell>(); "
        "    arr.Add(new Cell(1)); arr.Add(new Cell(2)); "
        "    arr.Set(0, new Cell(3)); "  // displaces Cell(1) → must dispose it
        "  end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// The replacement value is correct and exactly the live count remains.
BEZEL_TEST(owned_array_set_dispose_919, set_replaces_value_correctly) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  using var arr := new OwnedArray<Cell>(); "
        "  arr.Add(new Cell(1)); arr.Add(new Cell(2)); arr.Add(new Cell(3)); "
        "  arr.Set(1, new Cell(99)); "  // slot1 := 99, dispose old Cell(2)
        "  return arr[1].id + (100 if Cell.n = 3 else 0); "  // 99 + 100 = 199
        "end Main; end C;"), 199);
}
