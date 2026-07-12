// fix #758 / stdlib §5: SelectMany whose selector returns a LAZY LINQ chain
// (e.g. `(n) => arr.Where(...)`) — flatten correctly.
//
// Two defects, both fixed here:
//   (1) IR_FAIL: the lazy-LINQ-enumerable sentinel TypeRef lowered to i32 in
//       lowerTypeRef, so the selector lambda was declared with an i32 return
//       while its body returned a `ptr` ("return type does not match operand
//       type"). Fixed by lowering the sentinel to a pointer.
//   (2) WRONG_VALUE (unmasked by (1)): the SelectMany flatten trampoline wrapped
//       the selector result via `bz_lazy_from_array`, which mis-reads a lazy
//       enumerator struct as an array header — so a lazy inner yielded only a
//       garbage prefix (one element per outer). Fixed by returning the lazy
//       inner enumerator directly when the selector's return type is lazy.
//
// stdlib §5: `SelectMany(selector: (T) -> IEnumerable<U>)` flattens the inner
// sequences; the selector body may be any valid LINQ chain.

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

// §5: selector `(n) => arr.Where((m) => m <= n)` — a CAPTURING lazy chain.
// inner counts 1,2,3 → flattened total 6. (The #758 repro.)
BEZEL_TEST(selectmany_lazy_inner_758, capturing_where_chain_flattens) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr: Array<int> := [1, 2, 3]; "
        "    var count := 0; "
        "    for var x in arr.SelectMany((n) => arr.Where((m) => m <= n)) do "
        "      count := count + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;"), 6);
}

// §5: a NON-capturing lazy inner `(n) => arr.Where((m) => m <= 2)` — each inner
// is [1,2] → flattened total 6. (Isolates the flatten fix from capture.)
BEZEL_TEST(selectmany_lazy_inner_758, noncapturing_where_chain_flattens) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr: Array<int> := [1, 2, 3]; "
        "    var count := 0; "
        "    for var x in arr.SelectMany((n) => arr.Where((m) => m <= 2)) do "
        "      count := count + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;"), 6);
}

// §5: a lazy inner `Select` projection — sum of all products (1+2+3)*(1+2+3)=36
// confirms BOTH the flatten and the projected element values are correct.
BEZEL_TEST(selectmany_lazy_inner_758, select_chain_flattens_values) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr: Array<int> := [1, 2, 3]; "
        "    var sum := 0; "
        "    for var x in arr.SelectMany((n) => arr.Select((m) => m * n)) do "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end C;"), 36);
}

// §5: a multi-stage lazy inner chain `Where(...).Select(...)` — count 6.
BEZEL_TEST(selectmany_lazy_inner_758, chained_where_select_inner_flattens) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr: Array<int> := [1, 2, 3]; "
        "    var count := 0; "
        "    for var x in arr.SelectMany("
        "        (n) => arr.Where((m) => m <= n).Select((m) => m + 1)) do "
        "      count := count + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;"), 6);
}

// §5 regression guard: a selector returning a CONCRETE collection (not a lazy
// chain) still flattens via the array path — 3 x 3 = 9.
BEZEL_TEST(selectmany_lazy_inner_758, concrete_collection_selector_unaffected) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr: Array<int> := [1, 2, 3]; "
        "    var count := 0; "
        "    for var x in arr.SelectMany((n) => arr) do count := count + 1; end; "
        "    return count; "
        "  end Main; "
        "end C;"), 9);
}
