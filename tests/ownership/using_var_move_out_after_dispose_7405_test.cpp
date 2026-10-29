// Spec-derived regression tests for issue #7405 (campaign
// `using-heads`, U1 soundness: the `using var` pin and explicit Dispose).
//
// #7405 -- a `using var` can be moved out of after an explicit `Dispose()`.
//   Spec §28.3.7 *Move semantics for owned locals* (v0.350): "A `using var` is
//   owned but is additionally bound to its scope: it cannot be moved out of,
//   only borrowed from (axiom A5)." §28.10: "`using var` is a scope pin (axiom
//   A5)". A5 is a property of the BINDING, not of its state: §28.2's
//   dominating-kill warning governs a consume of a plain owned local and does
//   not lift the pin. Before the fix `a.Dispose(); R.Take(move a);` over a
//   `using var a` drew only the §28.2 warning ("move from a local `a` already
//   disposed"), and ran.
//
// Every REJECT row asserts the specific diagnostic (never "did not crash",
// which is vacuous for a use-after-free). Every ACCEPT row runs the program
// under the AOT/ASan oracle and asserts its stdout.

#include <string>
#include <string_view>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// The issue bodies' module, verbatim, with the method `Body` swapped out.
std::string probe(const std::string& body) {
    return std::string(R"(module Probe;

public sealed class Q
  private _id: int;
  public constructor(id: int)
    _id := id;
  end constructor;
  destructor
    Console.WriteLine($"D:{_id}");
  end destructor;
  public method Use()
    Console.WriteLine($"U:{_id}");
  end Use;
end Q;

public sealed class R
  private _id: int;
  public owned f: Q;
  public constructor(id: int, owned q: Q)
    _id := id;
    f := q;
  end constructor;
  destructor
    Console.WriteLine($"D:{_id}");
  end destructor;
  public method Use()
    Console.WriteLine($"U:{_id}");
  end Use;
  public static method Take(owned r: R)
    using var k := move r;
  end Take;
  public static method Body()
)") + body + R"(
  end Body;
end R;

public class Program
  public static method Main(): int
    R.Body();
    Console.WriteLine("END");
    return 0;
  end Main;
end Program;
)";
}

constexpr std::string_view kA5 = "cannot move out of `using var` `a`";

}  // namespace

// ---------------------------------------------------------------------------
// #7405 -- REJECT (§28.3 / §28.10, axiom A5, v0.350)
// ---------------------------------------------------------------------------

// The issue's literal repro.
BEZEL_TEST(using_var_pin_move_out_7405,
           move_of_using_var_after_explicit_dispose_rejected_spec_28_3_a5) {
    auto r = parseAndCheckOwnership(probe(R"(
    using var a := new R(2, new Q(1));
    a.Dispose();
    R.Take(move a);)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kA5));
}

// Sibling: the move-out into a plain `var` binding.
BEZEL_TEST(using_var_pin_move_out_7405,
           var_binding_move_of_disposed_using_var_rejected_spec_28_3_a5) {
    auto r = parseAndCheckOwnership(probe(R"(
    using var a := new R(2, new Q(1));
    a.Dispose();
    var b := move a;
    b.Use();)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kA5));
}

// Sibling: the move-out into another `using var` (an adoption, §28.10).
BEZEL_TEST(using_var_pin_move_out_7405,
           using_var_adoption_of_disposed_using_var_rejected_spec_28_3_a5) {
    auto r = parseAndCheckOwnership(probe(R"(
    using var a := new R(2, new Q(1));
    a.Dispose();
    using var b := move a;)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kA5));
}

// Control (the pre-existing rejection): without the `Dispose()` the same move
// was already the A5 error; the diagnostic is unchanged.
BEZEL_TEST(using_var_pin_move_out_7405,
           move_of_undisposed_using_var_still_rejected_spec_28_3_a5) {
    auto r = parseAndCheckOwnership(probe(R"(
    using var a := new R(2, new Q(1));
    R.Take(move a);)"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kA5));
}

// Over-reject mirror: the move-out of a PLAIN owned local after its explicit
// `Dispose()` is §28.2's dominating-kill WARNING, not an error (the binding is
// not a `using var`; v0.303, pinned by move_after_dominating_dispose_warns_4450).
BEZEL_TEST(using_var_pin_move_out_7405,
           move_of_disposed_plain_local_stays_a_warning_spec_28_2) {
    auto r = parseAndCheckOwnership(probe(R"(
    var a := new R(2, new Q(1));
    a.Dispose();
    R.Take(move a);)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Over-reject mirror: an explicit `Dispose()` of a `using var` is itself
// admitted (§28.10: a site-announced kill), and a different binding's
// Dispose() is untouched. Runs under ASan: each object disposed exactly once.
BEZEL_TEST(using_var_pin_move_out_7405,
           explicit_dispose_of_using_var_and_of_other_binding_accepted_spec_28_10) {
    auto r = runAotChecked(probe(R"(
    using var a := new R(2, new Q(1));
    var c := new R(4, new Q(3));
    c.Dispose();
    a.Dispose();)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("D:4\nD:3\nD:2\nD:1\nEND\n"));
}

