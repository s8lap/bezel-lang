// Codegen — a +1 `string` owned by a whole CONSTRUCT: a `for` iterable
// (§21.4), across `yield` / `await` (§29.3, §30.2), iterator abandonment
// (§29.7) and `yield break` (§29.3).
//
// §28.9 leaves a string's lifetime to the runtime "without user
// involvement"; the loop is the only holder of an iterable that is a fresh
// string, so the loop releases it — on every exit, and from the iterator's
// `Dispose` when the iterator is abandoned while suspended inside the loop.
// (The `case`-scrutinee spelling of the same boundary, #7643, is parked
// behind #8453 and has no row here.)
//
// Every row loops under `runAotChecked`, whose leak check is the oracle.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// #8653 (§29.3 / §21.4): a `for` over a string whose body yields. The loop's
// index and the string it walks must survive the suspend; before the fix the
// module did not verify ("Instruction does not dominate all uses"). 200x.
BEZEL_TEST(codegen_str_construct_release, string_for_keeps_loop_state_across_yield_8653_spec_29_3) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method W(s: string): IEnumerable<int> "
        "    for var c in s do "
        "      yield return 1; "
        "    end; "
        "    yield return 100; "
        "  end W; "
        "  public static method Main(): int "
        "    var i := 0; var tot := 0; "
        "    var s := \"abc\" + 7.ToString(); "
        "    while i < 200 do "
        "      i := i + 1; "
        "      for var v in C.W(s) do tot := tot + v; end; "
        "    end while; "
        "    if tot = 200 * 104 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}

// #8653 (§30.2 / §21.4): the `await` twin — the loop resumed on a fresh stack
// with its index reset, so the count came back wrong. 50x.
BEZEL_TEST(codegen_str_construct_release, string_for_keeps_loop_state_across_await_8653_spec_30_2) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static async method W(s: string): Task<int> "
        "    var n := 0; "
        "    for var c in s do "
        "      await Task.Delay(0); "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end W; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; var tot := 0; "
        "    var s := \"abc\" + 7.ToString(); "
        "    while i < 50 do "
        "      i := i + 1; "
        "      tot := tot + await C.W(s); "
        "    end while; "
        "    if tot = 50 * 4 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 1);
}
