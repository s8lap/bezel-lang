// #7458 (spec §7.5 + §28.3, v0.350): `c?.Count()` (and every other §5 terminal
// operator spelled with `?.`) over a NAMED `IEnumerable<T>?` local / parameter
// must not dispose the receiver.
//
// THE RULES.
//   §7.5   "`x?.M` returns null if `x` is null; otherwise it performs the
//          access." The access is the same access the `.` form performs.
//   §28.3  "An owned local is disposed exactly once on every control-flow path"
//          (auto-drop at scope end). A `?.` terminal call borrows the receiver;
//          it is not a consume position.
//
// THE DEFECT. lowerNullCondBuiltinCall lowers the non-null path as a synthetic
// `.` call over a symbol-less placeholder receiver. The vtable-backed
// IEnumerable materializer (materializeVtableIEnumerableToArray) decides
// "named local, owned elsewhere" from the receiver node's exprSymbol; the
// placeholder carried none, so it was classified as an unbound TEMPORARY and
// disposed by the terminal, then again by the named local's scope-end
// auto-drop: a heap-use-after-free (ASan) / SIGSEGV (rc 139) after the output
// was printed.
//
// Every row is runAotChecked (generated-code ASan + LSan) and loops the
// access 60x, so a double dispose or a leak cannot hide.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& members, const std::string& body) {
    return "module M; import System; import System.Linq; "
           "class P " +
           members +
           "  public static method Main(): int " +
           body + " return 0; end Main; end P;";
}

}  // namespace

// The issue's repro, verbatim shape, looped.
BEZEL_TEST(null_cond_vtable_ienumerable_7458, local_count_call_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "",
        "  var a: Array<int> := [1, 2, 3]; "
        "  var c: IEnumerable<int>? := a; "
        "  var sum := 0; "
        "  for var i in 1..60 do sum := sum + (c?.Count() ?? -1); end; "
        "  Console.WriteLine(c?.Count() ?? -1); "
        "  Console.WriteLine(sum); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n180\n");
}

// The other terminals share the materializer: Sum / First / Any / ToArray, on a
// non-null receiver, then the null receiver (nothing to dispose, -1 / false).
BEZEL_TEST(null_cond_vtable_ienumerable_7458, terminal_matrix_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "",
        "  var a: Array<int> := [1, 2, 3]; "
        "  var c: IEnumerable<int>? := a; "
        "  var n: IEnumerable<int>? := null; "
        "  var acc := 0; "
        "  for var i in 1..60 do "
        "    var t := c?.ToArray(); "
        "    acc := acc + (t?.Count ?? -100) + (c?.Sum() ?? -100) "
        "           + (c?.First() ?? -100) + (n?.Sum() ?? 0); "
        "    if (c?.Any() ?? false) and not (n?.Any() ?? false) then "
        "      acc := acc + 1000; "
        "    end; "
        "  end; "
        "  Console.WriteLine(acc); "
        "  Console.WriteLine(c?.Sum() ?? -1); "
        "  Console.WriteLine(n?.Count() ?? -1); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // per iteration: 3 + 6 + 1 + 0 + 1000 = 1010; 60 iterations = 60600.
    BEZEL_EXPECT_EQ(r.stdoutText, "60600\n6\n-1\n");
}

// A parameter receiver (the issue's second shape: the callee reads it).
BEZEL_TEST(null_cond_vtable_ienumerable_7458, parameter_receiver_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "  public static method Cnt(c: IEnumerable<int>?): int "
        "    return c?.Count() ?? -1; "
        "  end Cnt; ",
        "  var a: Array<int> := [1, 2, 3, 4]; "
        "  var c: IEnumerable<int>? := a; "
        "  var n: IEnumerable<int>? := null; "
        "  var sum := 0; "
        "  for var i in 1..60 do sum := sum + Cnt(c) + Cnt(n); end; "
        "  Console.WriteLine(sum); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // (4 + -1) * 60 = 180
    BEZEL_EXPECT_EQ(r.stdoutText, "180\n");
}

// A genuine TEMPORARY receiver keeps its dispose-the-temporary behavior: the
// symbol copy must not turn this into a leak. (`Mk()` hands out a fresh
// owned iterator frame.)
BEZEL_TEST(null_cond_vtable_ienumerable_7458, temporary_receiver_still_disposed_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "  public static method Mk(n: int): IEnumerable<int> "
        "    for var i in 1..n do yield return i; end; "
        "  end Mk; ",
        "  var sum := 0; "
        "  for var i in 1..60 do sum := sum + (Mk(4)?.Count() ?? -1); end; "
        "  Console.WriteLine(sum); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "240\n");
}
