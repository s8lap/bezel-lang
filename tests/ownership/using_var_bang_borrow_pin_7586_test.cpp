// `using var p := b!;` over a BORROW-state binding is a §28.10 borrow-pin of
// `b`'s source, exactly as `using var p := b;` is (issue #7586; spec §17.8 /
// §28.10, v0.350).
//
// THE GOVERNING TEXT.
//
//   §17.8 (v0.350) — "if `x` is borrowed, `x!` is borrowed with the same
//   source. ... Over a named owned binding, `x!` is therefore classified by
//   its position exactly as the bare read `x` is".
//
//   §28.10 *`using var` initialized from a borrow* (v0.350) — over "an
//   unmarked read of a **borrow-state** named binding (a plain parameter, a
//   borrowing alias or loop variable, ...)" the `using var` "does **not**
//   assume a disposal obligation" and "still **pins** the borrow's source for
//   the duration of the scope ... the consume-site freeze then governs every
//   consume of the source in that range — the frozen kills (a move, a
//   transfer, a reassignment of the binding) are compile errors, and the
//   **site-announced** kills stay unfrozen ...: an explicit `Dispose()` of the
//   source is permitted".
//
// THE BUG. `handleLocalDecl`'s bare-identifier pin arm did not peel the `!`,
// so the pin flag stayed clear for `using var p := b!;` and the consume-site
// freeze (which only scans flagged pins) never saw it:
// `var a := ...; var b := a; using var p := b!; R.Take(move a);` was
// accepted. Over a class-typed named OWNED binding, `using var p := a!;` is
// §28.10's ambiguity rejection, unchanged here.
//
// THE ORACLE. Reject rows pin the freeze diagnostic's head and holder; the
// pre-fix compiler accepted every one. Accept rows run the program and pin
// the exact destructor trace, so a disposal of the pin (a double free) or a
// lost disposal of the source (a leak) is distinguishable from the spec's
// answer.
//
// per §17.8, §28.10, v0.350

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// `body` is the body of `R.Body`, which `Main` calls twice.
std::string prog(const std::string& body) {
    return std::string(R"(module Probe;

public sealed class Q
  private _id: int;
  public constructor(id: int)
    _id := id;
  end constructor;
  destructor
    Console.WriteLine($"D:{_id}");
  end destructor;
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
  public static method UseP(prm: R?)
    using var p := prm!;
    p.Use();
  end UseP;
  public static method Body()
)") + body + R"(
  end Body;
end R;

public class Program
  public static method Main(): int
    R.Body();
    R.Body();
    Console.WriteLine("END");
    return 0;
  end Main;
end Program;
)";
}

void expectFreeze(const JitResult& r) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot consume `a` here"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "borrow held by `p`"));
}

void expectRun(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// --- REJECT: a frozen kill of the pinned source under `using var p := b!;` --

BEZEL_TEST(ownership_using_var_bang_borrow_pin_7586,
           spec_28_10_transfer_of_source_under_bang_alias_pin_rejected) {
    // The issue's repro. Pre-fix: accepted.
    expectFreeze(runAot(prog(R"(
    var a := new R(2, new Q(1));
    var b := a;
    using var p := b!;
    R.Take(move a);)")));
}

BEZEL_TEST(ownership_using_var_bang_borrow_pin_7586,
           spec_17_8_double_bang_alias_pin_is_the_same_pin) {
    // `b!!` is `b!` is `b` (§17.8). Pre-fix: accepted.
    expectFreeze(runAot(prog(R"(
    var a := new R(2, new Q(1));
    var b := a;
    using var p := b!!;
    R.Take(move a);)")));
}

BEZEL_TEST(ownership_using_var_bang_borrow_pin_7586,
           spec_28_10_reassignment_of_source_under_bang_alias_pin_rejected) {
    // "a reassignment of the binding" is a frozen kill. Pre-fix: accepted.
    expectFreeze(runAot(prog(R"(
    var a := new R(2, new Q(1));
    var b := a;
    using var p := b!;
    a := new R(6, new Q(5));)")));
}

BEZEL_TEST(ownership_using_var_bang_borrow_pin_7586,
           spec_28_10_marked_binding_move_of_source_under_bang_alias_pin_rejected) {
    // "a move" is a frozen kill. Pre-fix: accepted.
    expectFreeze(runAot(prog(R"(
    var a := new R(2, new Q(1));
    var b := a;
    using var p := b!;
    var z := move a;)")));
}

// --- ACCEPT twins ------------------------------------------------------------

BEZEL_TEST(ownership_using_var_bang_borrow_pin_7586,
           spec_28_10_transfer_of_source_after_bang_alias_pin_scope_accepted) {
    // The accepting twin of the issue's repro: the transfer follows the pin's
    // `do ... end` scope, so the freeze range has ended. The pin disposes
    // nothing; `Take` disposes `a` (and its owned `f`) exactly once per call.
    expectRun(runAot(prog(R"(
    var a := new R(2, new Q(1));
    var b := a;
    do
      using var p := b!;
      p.Use();
    end;
    R.Take(move a);)")),
              "U:2\nD:2\nD:1\nU:2\nD:2\nD:1\nEND\n");
}

BEZEL_TEST(ownership_using_var_bang_borrow_pin_7586,
           spec_28_10_explicit_dispose_of_source_under_bang_alias_pin_accepted) {
    // "an explicit `Dispose()` of the source is permitted" (a site-announced
    // kill stays unfrozen). Exactly one disposal per call: the pin's scope end
    // disposes nothing.
    expectRun(runAot(prog(R"(
    var a := new R(2, new Q(1));
    var b := a;
    using var p := b!;
    a.Dispose();)")),
              "D:2\nD:1\nD:2\nD:1\nEND\n");
}

BEZEL_TEST(ownership_using_var_bang_borrow_pin_7586,
           spec_28_10_bang_pin_of_plain_parameter_disposes_nothing_control) {
    // Control (passes before and after): `using var p := prm!;` over a plain
    // parameter is a borrow-pin; the caller's `a` is disposed once, by the
    // caller.
    expectRun(runAot(prog(R"(
    var a := new R(2, new Q(1));
    R.UseP(a);)")),
              "U:2\nD:2\nD:1\nU:2\nD:2\nD:1\nEND\n");
}
