// Codegen — issue #2739: `await using var` over a borrowing builtin collection
// literal (`await using var xs: Array<int> := [k, k, k]`) crashed at run time
// with a call through a NULL function pointer (`pc 0x0`) or an LLVM Verification
// Failure. Root cause: registerUsingDisposal's `isAwaitUsing` arm (line 14641)
// registered a Disposal with `asyncDispose=true, disposeAsyncFn=null` — there is
// no user DisposeAsync to resolve for the declaration-less builtin heads (Array /
// Dictionary / LinkedList / IList / etc., classOfLocal returns null) — then
// emitDisposals fell back to emitDisposeAndReclaim → emitDynamicDispose, which
// loads word 0 of the bz_array_t* / bz_dict_t* / bz_llist_t* as a class vtable
// pointer and calls vtable slot 2. The plain `using var` form is clean because
// it routes through the linkedList / listInterface / collection / dict disposal
// branches below (bz_array_free / bz_llist_free / bz_dict_free); the `await`
// form has no user-async-disposable shape (no DisposeAsync to await), so apply
// the same synchronous collection disposal.
//
// The control `using var xs: Array<int> := [k, k, k]` already worked — those
// tests pin the sync form (negative controls) alongside the await form (the bug
// being fixed). §30.5 / §30.6, v0.240.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Primary reproducer: `await using var xs: Array<int> := [k, k, k]` in an
// async method body — must NOT crash and must return the right value. Sum(i)
// returns 3*i; total over 1..10 is sum(3*i) = 3 * 55 = 165.
BEZEL_TEST(await_using_borrowing_collection_literal_2739,
           await_using_var_array_int_literal_no_crash) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Sum(k: int): Task<int> "
        "    await using var xs: Array<int> := [k, k, k]; "
        "    var n := 0; "
        "    for var v in xs do "
        "      n := n + v; "
        "    end; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..10 do "
        "      total := total + C.Sum(i).Result; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"), 165);
}

// Negative control: same program with plain `using var` — was clean before the
// fix; stays clean after. Same expected return (165).
BEZEL_TEST(await_using_borrowing_collection_literal_2739,
           using_var_array_int_literal_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Sum(k: int): Task<int> "
        "    using var xs: Array<int> := [k, k, k]; "
        "    var n := 0; "
        "    for var v in xs do "
        "      n := n + v; "
        "    end; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..10 do "
        "      total := total + C.Sum(i).Result; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"), 165);
}

// Aggregate root + iteration: `Array<Array<int>>` literal — same
// borrowing-collection crash shape per the issue's repro list.
BEZEL_TEST(await_using_borrowing_collection_literal_2739,
           await_using_var_nested_collection_no_crash) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Sum(s: string): Task<int> "
        "    await using var rows: Array<Array<int>> := [[1, 2], [3, 4]]; "
        "    var n := 0; "
        "    for var row in rows do "
        "      for var v in row do n := n + v; end; "
        "    end; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    return C.Sum(\"x\").Result; "
        "  end Main; "
        "end C;"), 10);
}

// Dictionary literal in await-using position — exercises the dict arm.
BEZEL_TEST(await_using_borrowing_collection_literal_2739,
           await_using_var_dictionary_literal_no_crash) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    await using var d: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
        "    return d[\"a\"] + d[\"b\"]; "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run().Result; "
        "  end Main; "
        "end C;"), 3);
}

// Array<string> literal — exercises per-element string release.
BEZEL_TEST(await_using_borrowing_collection_literal_2739,
           await_using_var_array_string_literal_no_crash) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    await using var xs: Array<string> := [\"ab\", \"cd\"]; "
        "    return xs.Count; "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run().Result; "
        "  end Main; "
        "end C;"), 2);
}
