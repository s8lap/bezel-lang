// fix #8837 / §28.3 closure bullet, provenance clause 1 + the sink rule, K3.13
// (spec v0.359): a returned closure capturing an ALIAS `b` (`var b := t;`) of an
// owned scope-bound local `t`.
//
// K3.13: "`var b := a;` — a borrow: an unmarked read never consumes". §28.3's
// closure bullet, provenance clause 1: "A lambda literal contributes the sources
// of the borrows it captures — a captured borrowed local or parameter that
// binding's source; a captured owned local its enclosing scope". The sink rule:
// "While its source set is non-empty, a function value ... may not be returned
// from the enclosing method". So `return lambda() => b.v;` carries `t` and is
// rejected exactly as the direct `return lambda() => t.v;` is.
//
// MEASURED on r30-c1 (0d5d80a58): both rows below compiled, and the issue's
// repro read `t` after its scope-end disposal (`heap-use-after-free ... in
// lam$0` under --sanitize=address). The direct-capture leg of
// checkClosureEscapesOnReturn keys on an Owned capture; `b` is Borrowed, and
// borrowSourceForExpr's closure shape (Shape 3) never read a captured borrow's
// own source, so a holder `f` of such a lambda carried nothing either.
//
// Rows 1-2 are red on the base and assert the specific diagnostic at the
// captured identifier / the returned holder. Row 3 is the one boundary control:
// the same alias capture whose closure does NOT escape (invoked before `t`'s
// scope ends) must still compile and run ASan-clean 200x. It is red under the
// plausible wrong fix that rejects a lambda capturing a borrowed local at its
// formation (or whenever its holder carries a source) rather than at the escape.
//
// per §28.3, K3.13, v0.359, #8837

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ROW 1 — the issue's own repro: `return lambda() => b.v;` over `var b := t;`.
// The captured `b` is at line 12, column 24.
BEZEL_TEST(closure_captures_alias_of_owned_local_8837,
           returned_lambda_capturing_alias_of_owned_local_rejected_spec_28_3_v0_359) {
    const std::string src = R"(
module P;
import System;
class Cell
  public v: int;
  public constructor(x: int) this.v := x; end constructor;
end Cell;
class H
  public method F(): () -> int
    var t := new Cell(8);
    var b := t;
    return lambda() => b.v;
  end F;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var f := h.F();
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":12:24: returned closure captures `b`, a borrow of `t` whose source "
           "is disposed at scope end"));
}

// ROW 2 — the same capture bound to a holder first: `var f := lambda() => b.v;
// return f;`. The holder carries the captured borrow's source (clause 1, then
// clause 6 at the read), so the return of `f` (line 13, column 12) is the
// indirect escape of `t`.
BEZEL_TEST(closure_captures_alias_of_owned_local_8837,
           returned_holder_of_lambda_capturing_alias_rejected_spec_28_3_v0_359) {
    const std::string src = R"(
module P;
import System;
class Cell
  public v: int;
  public constructor(x: int) this.v := x; end constructor;
end Cell;
class H
  public method F(): () -> int
    var t := new Cell(8);
    var b := t;
    var f := lambda() => b.v;
    return f;
  end F;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var f := h.F();
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":13:12: returned `f` carries a borrow of `t` whose source is "
           "disposed at scope end"));
}

// ROW 3 (boundary control) — the alias capture does not escape: the closure is
// invoked while `t` is alive and only an `int` leaves the method. §28.3's
// closure bullet admits binding and invoking it ("it may be bound to a local
// ... it may be immediately invoked"); 200 calls under ASan/LSan.
BEZEL_TEST(closure_captures_alias_of_owned_local_8837,
           non_escaping_lambda_capturing_alias_runs_clean_spec_28_3_v0_359) {
    const std::string src = R"(
module P;
import System;
class Cell
  public v: int;
  public constructor(x: int) this.v := x; end constructor;
end Cell;
class H
  public method F(): int
    var t := new Cell(8);
    var b := t;
    var f := lambda() => b.v;
    return f();
  end F;
end H;
public class Program
  public static method Main(): int
    var h := new H();
    var acc := 0;
    for var i in 0..<200 do acc := acc + h.F(); end;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1600\n");
}

}  // namespace
