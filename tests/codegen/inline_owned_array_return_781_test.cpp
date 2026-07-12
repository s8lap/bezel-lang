// fix #781 / §15.3: a method that `return`s an inline-array collection literal
// materializes the array into the caller's local.
//
// §15.3: `[N owned]T?` / `[N]T` are fixed-size inline arrays. A method declared
// to return one returns the `[N x elem]` aggregate BY VALUE. The bug:
// lowerExpr(CollectionLiteralExpr) returns null and the return path only had a
// HEAP-collection materializer, so `return [new Cell(), new Cell()]` returned an
// undef/zero array — the caller's `var a := Make()` came back all-null
// (WRONG_VALUE). The fix builds the inline-array aggregate in the return path.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §15.3: the returned array's first slot is materialized (not null). (The repro.)
BEZEL_TEST(inline_owned_array_return_781, returned_array_first_slot_materialized) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class Cell public n: int := 5; end Cell; "
        "class C "
        "  public static method Make(): [2 owned]Cell? "
        "    return [new Cell(), new Cell()]; "
        "  end Make; "
        "  public static method Main(): int "
        "    var a := C.Make(); "
        "    return 0 if a[0] = null else a[0]!.n; "  // 5
        "  end Main; "
        "end C;"), 5);
}

// §15.3: EVERY slot is materialized with its element's value, in order.
BEZEL_TEST(inline_owned_array_return_781, all_slots_materialized_in_order) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class Cell public n: int := 0; "
        "  public constructor(v: int) n := v; end; end Cell; "
        "class C "
        "  public static method Make(): [3 owned]Cell? "
        "    return [new Cell(10), new Cell(20), new Cell(30)]; "
        "  end Make; "
        "  public static method Main(): int "
        "    var a := C.Make(); "
        "    return a[0]!.n + a[1]!.n + a[2]!.n; "  // 60
        "  end Main; "
        "end C;"), 60);
}

// §15.3 / §28.3 soundness: the returned array's owned slots are disposed EXACTLY
// ONCE when the caller's local goes out of scope — no leak (would be 0), no
// double-free (would be > 2 or a crash).
BEZEL_TEST(inline_owned_array_return_781, returned_owned_slots_disposed_once) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Cell public static disposed: int := 0; public n: int := 0; "
        "  public constructor(v: int) n := v; end; "
        "  destructor Cell.disposed := Cell.disposed + 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Make(): [2 owned]Cell? "
        "    return [new Cell(1), new Cell(2)]; "
        "  end Make; "
        "  public static method Main(): int "
        "    do var a := C.Make(); var x := a[0]!.n; end; "
        "    return Cell.disposed; "  // 2
        "  end Main; "
        "end C;"), 2);
}
