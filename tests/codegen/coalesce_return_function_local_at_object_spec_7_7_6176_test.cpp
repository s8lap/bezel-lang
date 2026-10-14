// Regression pins for issue #6176 (spec §7.7 "Null-coalescing", §27.5, §28.9;
// all v0.338): a function-valued local returned through `??` where the
// declared return type is not a function type.
//
// §7.7: "`a ?? b` returns `a` if non-null, else `b`." The value of
// `return nn ?? e;` is therefore the very closure cell the local `nn` holds.
// Before the fix the closure escape walk read both operands of every binary
// operator as non-consuming, so `nn` kept its scope-end `bz_closure_release`;
// at a function-typed return type the `return` arm mints the caller's
// reference and that release balances, but at `Object` nothing is minted, so
// the method freed the cell and returned it — a heap-use-after-free at the
// caller's first read.
//
// ORACLE. `runAotCheckedLenient`: generated-code AddressSanitizer always on,
// LeakSanitizer off. A function value handed on from an `Object` location is
// not reclaimed (#5967) — `return nn;` at an `Object` return type leaks the
// same one cell per call — so these rows cannot be leak-clean until #5967 is;
// what they pin is that the returned cell is live. Each program is looped
// and asserts stdout. The last row is the function-typed return venue, which
// was balanced before the fix and must stay leak-clean (`runAotChecked`).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// `F` is the method under test; `Main` calls it 100 times and compares the
// result with itself and with a string, both as `Object`.
std::string program(const std::string& f) {
    return std::string(
               "module M; "
               "import System; "
               "public class Program "
               "  public static method Pick(some: bool): ((int) -> int)? "
               "    if some then return lambda(x: int) => x * 2; end; "
               "    return null; "
               "  end Pick; ") +
           f +
           "  public static method Main() "
           "    var t: int := 0; "
           "    var i: int := 0; "
           "    while i < 100 do "
           "      var o := Program.F(true); "
           "      var p: Object := \"b\"; "
           "      if o = o then t := t + 1; end; "
           "      if o = p then t := t + 10; end; "
           "      i := i + 1; "
           "    end; "
           "    Console.WriteLine(t); "
           "  end Main; "
           "end Program;";
}

void expectLiveResult(const std::string& f) {
    auto r = runAotCheckedLenient(program(f));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

}  // namespace

// §7.7, well-typed: the local is declared `Object?`, the fallback a string.
BEZEL_TEST(coalesce_return_function_local_at_object_spec_7_7_6176,
          object_local_coalesce_string_returns_a_live_cell_7_7) {
    expectLiveResult(
        "  public static method F(some: bool): Object "
        "    var nn: Object? := Program.Pick(some); "
        "    return nn ?? \"b\"; "
        "  end F; ");
}

// §7.7 / §17.12, well-typed: a nullable function local, `?? throw`.
BEZEL_TEST(coalesce_return_function_local_at_object_spec_7_7_6176,
          function_local_coalesce_throw_returns_a_live_cell_7_7) {
    expectLiveResult(
        "  public static method F(some: bool): Object "
        "    var nn := Program.Pick(some); "
        "    return nn ?? throw new InvalidOperationException(\"x\"); "
        "  end F; ");
}

// §7.7: the RIGHT operand is the local, selected because the left is null.
BEZEL_TEST(coalesce_return_function_local_at_object_spec_7_7_6176,
          right_operand_local_returns_a_live_cell_7_7) {
    expectLiveResult(
        "  public static method F(some: bool): Object "
        "    var a: Object? := null; "
        "    var d: (int) -> int := lambda(x: int) => x + 1; "
        "    return a ?? d; "
        "  end F; ");
}

// §7.7: "composes through the right-associative chain" — the local is the
// middle operand of `a ?? nn ?? throw …`.
BEZEL_TEST(coalesce_return_function_local_at_object_spec_7_7_6176,
          chain_operand_local_returns_a_live_cell_7_7) {
    expectLiveResult(
        "  public static method F(some: bool): Object "
        "    var a: ((int) -> int)? := null; "
        "    var nn := Program.Pick(some); "
        "    return a ?? nn ?? throw new InvalidOperationException(\"x\"); "
        "  end F; ");
}

// The issue's repro with the one change that makes it well-typed (`nn`
// declared `Object?`; the issue spells `nn ?? "b"` over a nullable function
// local, which §7.7 does not type — #6214): the result is read by an
// interpolation hole, which needs the function value's `Object` view
// (#5978) as well as a live cell.
BEZEL_TEST(coalesce_return_function_local_at_object_spec_7_7_6176,
          issue_repro_interpolated_result_is_live_7_7) {
    const std::string src =
        "module M; "
        "import System; "
        "public class Program "
        "  public static method Pick(some: bool): ((int) -> int)? "
        "    if some then return lambda(x: int) => x * 2; end; "
        "    return null; "
        "  end Pick; "
        "  public static method F(some: bool): Object "
        "    var nn: Object? := Program.Pick(some); "
        "    return nn ?? \"b\"; "
        "  end F; "
        "  public static method Main() "
        "    var n: int := 0; "
        "    for var i in 0..100 do "
        "      var s := $\"{Program.F(true)}\"; "
        "      var u := $\"{Program.F(false)}\"; "
        "      if s.Length > 0 and u = \"b\" then n := n + 1; end; "
        "    end for; "
        "    Console.WriteLine(n); "
        "  end Main; "
        "end Program;";
    auto r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("101\n"));   // 0..100 inclusive
}

// Control (passes before the fix too): at a FUNCTION-typed return type the
// `return` arm mints the caller's reference, so the local keeps its scope-end
// release and the round trip stays balanced — leak-clean under LSan.
BEZEL_TEST(coalesce_return_function_local_at_object_spec_7_7_6176,
          function_typed_return_stays_balanced_and_leak_clean_7_7) {
    const std::string src =
        "module M; "
        "import System; "
        "public class Program "
        "  public static method Pick(some: bool): ((int) -> int)? "
        "    if some then return lambda(x: int) => x * 2; end; "
        "    return null; "
        "  end Pick; "
        "  public static method F(some: bool): (int) -> int "
        "    var nn := Program.Pick(some); "
        "    return nn ?? throw new InvalidOperationException(\"x\"); "
        "  end F; "
        "  public static method Main() "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 100 do "
        "      var g := Program.F(true); "
        "      t := t + g(1); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(t); "
        "  end Main; "
        "end Program;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}
