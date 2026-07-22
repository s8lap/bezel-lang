// Codegen — issue #919 (discovery sweep #7) / fix #920/#1112: the indexer
// setter `arr[i] := v` on OwningArray<class T> must dispose the displaced
// (owned) old element. (The `.Set(i, v)` method form that triggered the
// original leak has been removed from the collection surface per §4.8/§4.9
// v0.155 — the indexer is the spec-declared write surface.) §28.11 / §4.5.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kCell =
    "class Cell public static n: int := 0; public id: int; "
    "  public constructor(i: int) Cell.n:=Cell.n+1; id:=i; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// The replaced element is disposed (no leak): two Adds + one indexer-set, all disposed.
BEZEL_TEST(owned_array_set_dispose_919, indexer_set_disposes_displaced_element) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    using var arr := new OwningArray<Cell>(); "
        "    arr.Add(new Cell(1)); arr.Add(new Cell(2)); "
        "    arr[0] := new Cell(3); "  // displaces Cell(1) → must dispose it
        "  end; "
        "  return Cell.n; "  // 0
        "end Main; end C;"), 0);
}

// The replacement value is correct and exactly the live count remains.
BEZEL_TEST(owned_array_set_dispose_919, indexer_set_replaces_value_correctly) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  using var arr := new OwningArray<Cell>(); "
        "  arr.Add(new Cell(1)); arr.Add(new Cell(2)); arr.Add(new Cell(3)); "
        "  arr[1] := new Cell(99); "  // slot1 := 99, dispose old Cell(2)
        "  return arr[1].id + (if Cell.n = 3 then 100 else 0); "  // 99 + 100 = 199
        "end Main; end C;"), 199);
}
