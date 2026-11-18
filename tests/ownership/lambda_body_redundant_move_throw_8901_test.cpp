// fix #8901 / §28.3 K3.13 (marker economy) / K3.8 (throw operand) + §31.1
// *The throw operand* (spec v0.359): `throw move c;` inside a CLOSURE body.
//
// §31.1: "`throw x` is a source-visible kill — the keyword itself is the
// announcement (K3.13), so the transfer marker is not written here — `throw
// move x` is rejected at the marker-free position (§28.3) — the same no-marker
// rule as an `owned` return". The consume table's K3.8 row ("the operand of
// `throw`") has Spelling = "none": the `throw` keyword IS the transfer
// marker.
//
// The METHOD body rejected the spelling — "the `move` marker is redundant on a
// `throw` operand — the `throw` keyword already announces the ownership
// transfer" — but ownership's statement walks do not enter a lambda body, so
// the same statement inside a closure was ADMITTED (MEASURED on r30-c1 @
// af51cca67: bezelc rc=0, the program ran and printed "caught"). The marker
// economy is a property of the `throw` keyword, not of the scope the throw is
// written in.
//
// Each row is red on the base, asserts the specific diagnostic AND its
// location (the `move` operand), and is the issue's own repro shape (row 1
// verbatim). The METHOD-body twin and the closure's own `throw c` (the legal
// spelling) are corpus candidates, not rows: they are green on both arms.
//
// per §28.3, §31.1, v0.359, #8901

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kR = R"(
module M;
public class R: Exception
  public constructor() end constructor;
end R;
)";

// ROW 1 — the issue's own repro: a closure throws `move c` of its OWN owned
// local (the `move` operand is at line 9, column 58).
BEZEL_TEST(lambda_body_redundant_move_throw_8901,
           block_closure_throw_move_of_own_owned_local_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do var c := new R(); throw move c; end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":9:58: the `move` marker is redundant on a `throw` operand"));
}

// ROW 2 — the closure's OWN `owned` PARAMETER: the same rule (the marker is
// redundant whatever the source of the operand is). The `move` operand is at
// line 9, column 50.
BEZEL_TEST(lambda_body_redundant_move_throw_8901,
           block_closure_throw_move_of_own_owned_parameter_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(owned c: R): int => do throw move c; end;
    try _ := f(new R()); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":9:50: the `move` marker is redundant on a `throw` operand"));
}

// ROW 3 — the `!` spelling: §17.8 classifies `x!` by its position exactly as
// the bare read `x`, so `throw move c!` is the same question (the `move`
// operand is at line 9, column 58).
BEZEL_TEST(lambda_body_redundant_move_throw_8901,
           block_closure_throw_move_bang_of_own_owned_local_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do var c := new R(); throw move c!; end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":9:58: the `move` marker is redundant on a `throw` operand"));
}

}  // namespace
