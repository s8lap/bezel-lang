// fix #6967 (stdlib §4.19, spec §28.3, v0.340): `FrozenArray<string>.From(src)`
// copies. `tryLowerFrozenArrayCall::hasFreshStorage` treated EVERY CallExpr
// source as a fresh owned rvalue and freed it after the copy; for a call that
// returns a BORROW (`Peek(): Array<string>`) that frees the holder's array and
// the holder frees it again: heap-use-after-free (MEASURED by the r34-6924
// reviewer on c0a1928b and on #6924's tip). Freshness is now read from the
// callee's declared return ownership (`sema::methodReturnsOwned`, the
// predicate the other call-result sites use): an `owned` return is fresh, a
// borrow return is retain-copied and never freed.
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
    "public class H\n"
    "  public owned Xs: Array<string>;\n"
    "  public constructor() Xs := new Array<string>(); Xs.Add(\"f\" + \"1\"); "
    "Xs.Add(\"f\" + \"2\"); end constructor;\n"
    "  public method Peek(): Array<string> return Xs; end Peek;\n"
    "  public property View: Array<string> => Xs;\n"
    "  public method Own(): owned Array<string>\n"
    "    var a := new Array<string>(); a.Add(\"o\" + \"1\"); a.Add(\"o\" + \"2\"); "
    "return a;\n"
    "  end Own;\n"
    "  private static owned readonly _cache: Array<string> := [\"s\" + \"1\", \"s\" + \"2\"];\n"
    "  public static property Cache: Array<string> => _cache;\n"
    "  public static method Lit(): owned Array<string>\n"
    "    return [\"l\" + \"1\", \"l\" + \"2\"];\n"
    "  end Lit;\n"
    "end H;\n";

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

// Row 1 — the issue's repro: a borrow-returning instance method.
BEZEL_TEST(frozen_array_from_borrow_call_source_6967,
           borrow_returning_instance_method_spec_28_3) {
    expectClean(runAotChecked(program(
        "using var h := new H(); using var x := FrozenArray<string>.From(h.Peek()); "
        "Console.WriteLine($\"{x.Length} {x[1]} {h.Xs[1]}\");")),
        repeatLine("2 f2 f2", 100));
}

// Row 2 — a static property getter re-exposing a `static owned` field (the
// only static borrow-return form §28.3 admits; a static METHOD returning a
// borrow is rejected, "Factory methods and sourceless returns").
BEZEL_TEST(frozen_array_from_borrow_call_source_6967,
           borrow_static_property_of_static_field_spec_28_3) {
    expectClean(runAotChecked(program(
        "using var h := new H(); using var x := FrozenArray<string>.From(H.Cache); "
        "Console.WriteLine($\"{x.Length} {x[0]} {H.Cache[0]}\");")),
        repeatLine("2 s1 s1", 100));
}

// Row 3 — control: an owned-returning method takes no extra retain (LSan).
BEZEL_TEST(frozen_array_from_borrow_call_source_6967,
           owned_returning_method_takes_no_extra_retain_spec_28_3) {
    expectClean(runAotChecked(program(
        "using var h := new H(); using var x := FrozenArray<string>.From(h.Own()); "
        "Console.WriteLine($\"{x.Length} {x[1]}\");")),
        repeatLine("2 o2", 100));
}

// Row 4 — a property getter returning a borrow.
BEZEL_TEST(frozen_array_from_borrow_call_source_6967,
           borrow_property_getter_spec_28_3) {
    expectClean(runAotChecked(program(
        "using var h := new H(); using var x := FrozenArray<string>.From(h.View); "
        "Console.WriteLine($\"{x.Length} {x[1]} {h.Xs[1]}\");")),
        repeatLine("2 f2 f2", 100));
}

// Row 5 — a call returning a fresh Array literal (owned).
BEZEL_TEST(frozen_array_from_borrow_call_source_6967,
           call_returning_fresh_array_literal_spec_28_3) {
    expectClean(runAotChecked(program(
        "using var x := FrozenArray<string>.From(H.Lit()); "
        "Console.WriteLine($\"{x.Length} {x[1]}\");")),
        repeatLine("2 l2", 100));
}
