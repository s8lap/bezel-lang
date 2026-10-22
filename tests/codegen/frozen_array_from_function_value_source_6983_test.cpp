// fix #6983 (stdlib §4.19, spec §28.3 / §27.1, v0.340): `FrozenArray<string>
// .From(f())` where `f` is a FUNCTION VALUE. #6967 read freshness from a
// resolved MethodSymbol; a call through a function value kept the "fresh
// rvalue" reading, so `From` freed the borrow `f` returned and the holder freed
// it again (heap-use-after-free). Freshness of such a call is now read from the
// callee's function type: an `owned` return slot (`() -> owned T`) is fresh;
// a lambda literal bound by `const` is judged by its expression body (sema
// leaves funcReturnOwned unset on a lambda literal's own type); any other plain
// function value returns a borrow.
//
// Every row loops 100x on `runAotChecked` and asserts exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string repeatLine(const std::string& line, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += line + "\n";
    return s;
}

void expectClean(const JitResult& r, const std::string& expectedStdout) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::cout << "  diag: " << d.message << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedStdout);
}

constexpr const char* kLib =
    "public class Prov\n"
    "  public owned Xs: Array<string>;\n"
    "  public constructor() Xs := new Array<string>(); Xs.Add(\"f\" + \"1\"); "
    "Xs.Add(\"f\" + \"2\"); end constructor;\n"
    "end Prov;\n"
    "public class M\n"
    "  public static method MkArr(): owned Array<string>\n"
    "    var a := new Array<string>(); a.Add(\"a\" + \"x\"); a.Add(\"b\" + \"y\"); "
    "a.Add(\"c\" + \"z\");\n"
    "    return a;\n"
    "  end MkArr;\n"
    "end M;\n"
    "public class U\n"
    "  public static method Run(f: () -> Array<string>): owned FrozenArray<string>\n"
    "    return FrozenArray<string>.From(f());\n"
    "  end Run;\n"
    "  public static method RunOwned(f: () -> owned Array<string>): owned "
    "FrozenArray<string>\n"
    "    return FrozenArray<string>.From(f());\n"
    "  end RunOwned;\n"
    "end U;\n";

std::string program(const std::string& body) {
    return std::string("module Loc;\n") + kLib +
           "class C\n"
           "  public static method Main(): int\n"
           "    var i := 0;\n"
           "    while i < 100 do\n"
           "      " + body + "\n"
           "      i += 1;\n"
           "    end;\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// Row 1 — the issue's repro: a lambda returning a borrow, bound by `const`.
BEZEL_TEST(frozen_array_from_function_value_source_6983,
           borrow_returning_lambda_local_spec_28_3) {
    expectClean(runAotChecked(program(
        "using var h := new Prov(); const f := lambda() => h.Xs; "
        "using var x := FrozenArray<string>.From(f()); "
        "Console.WriteLine($\"{x.Length} {x[1]} {h.Xs[1]}\");")),
        repeatLine("2 f2 f2", 100));
}

// Row 2 — the issue's repro: a plain `() -> Array<string>` parameter.
BEZEL_TEST(frozen_array_from_function_value_source_6983,
           borrow_returning_function_parameter_spec_28_3) {
    expectClean(runAotChecked(program(
        "using var h := new Prov(); using var x := U.Run(lambda() => h.Xs); "
        "Console.WriteLine($\"{x.Length} {x[1]} {h.Xs[1]}\");")),
        repeatLine("2 f2 f2", 100));
}

// Row 3 — control: a lambda whose body is an owned producer stays fresh (LSan).
BEZEL_TEST(frozen_array_from_function_value_source_6983,
           owned_returning_lambda_local_stays_fresh_spec_28_3) {
    expectClean(runAotChecked(program(
        "const f := lambda() => M.MkArr(); "
        "using var x := FrozenArray<string>.From(f()); "
        "Console.WriteLine($\"{x.Length} {x[1]}\");")),
        repeatLine("3 by", 100));
}

// Row 4 — control: a `() -> owned Array<string>` parameter stays fresh (LSan).
BEZEL_TEST(frozen_array_from_function_value_source_6983,
           owned_function_type_parameter_stays_fresh_spec_27_1) {
    expectClean(runAotChecked(program(
        "using var x := U.RunOwned(lambda() => M.MkArr()); "
        "Console.WriteLine($\"{x.Length} {x[1]}\");")),
        repeatLine("3 by", 100));
}
