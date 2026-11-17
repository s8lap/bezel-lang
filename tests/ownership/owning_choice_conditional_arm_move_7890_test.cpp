// An if-expression arm that names an OWNING-CHOICE local moves it on the
// selected path, whether the arm is written `o` or `o!` (issue #7890; spec
// §20 *Ownership*, §17.8, §28.3 K7.4, §35.4, v0.357).
//
// THE GOVERNING TEXT (docs/Bezel_Language_Spec.md, v0.357).
//
//   §20 *Ownership* — "When the branches are of a move-only value type
//   (§15.3), a branch naming such a binding classifies by the **position the
//   whole expression occupies**, exactly as a bare read of the binding does
//   ...: in an owned-consuming position — a binding, an assignment, a plain or
//   `owned` argument, a `return`, an owning slot — the **selected** branch's
//   value moves ..., the binding joining per-path consumed after the construct
//   (K7.4)".
//
//   §17.8 — "Over a named owned binding, `x!` is therefore classified by its
//   position exactly as the bare read `x` is".
//
//   §28.3, state 4 — "maybe-consumed — the K7 join of owned with moved-from
//   ...; unreadable until reassigned ...; scope-end disposal is per-path."
//
// THE BUG (#7890). The moved-from zero-store for the selected branch looked
// for a bare identifier arm, so `(if c then o! else OC.Empty)` stored nothing:
// the new holder and `o` both disposed the payload. MEASURED at r30-c1
// 46219d9ba under ASan on the first row: `heap-use-after-free`. The ownership
// pass did not join `o` maybe-consumed either, so a read of `o` after the
// construct was accepted (K7.4 says it is an error).
//
// The async rows: a maybe-consumed move-only local is per-path in an `async`
// body as well (§28.3.10: "The rule is uniform across body kinds"). The
// statement form `if c then x := o; end if;` was the hard "would be dropped
// here without disposal" reject there, and the expression form would have
// inherited it.
//
// Every row below fails at r30-c1 46219d9ba.
//
// per §20, §17.8, §28.3, §28.3.10, §35.4, v0.357

#include <string>

#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains, ...
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kPrelude = R"BZL(module M;
public class Res
  public static Count: int := 0;
  public N: int := 7;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;
public choice OC Text(owned r: Res), Empty; end OC;
public choice Opt<T> Some(owned v: T), None; end Opt;
public class Hold
  public owned G: OC? := null;
  public owned F: OC := OC.Empty;
  public constructor() end constructor;
end Hold;
public class Box<T>
  public owned V: T;
  public constructor(owned v: T) V := v; end constructor;
end Box;
public class Sink
  public static method Take(v: OC): int => 1;
end Sink;
)BZL";

std::string prog(const std::string& body, const std::string& members = "") {
    return std::string(kPrelude) + "public class Program\n" + members +
           "  public static method T(i: int): int\n" + body +
           "\n  end T;\n"
           "  public static method Main(): int\n"
           "    var s := 0;\n"
           "    for var i in 0..<100 do\n"
           "      s := s + T(i);\n"
           "    end;\n"
           "    Console.WriteLine($\"{Res.Count} {s}\");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// The `async` twin: `T` is an async method and `Main` awaits it.
std::string asyncProg(const std::string& body) {
    return std::string(kPrelude) + "public class Program\n"
           "  public static async method T(i: int): Task<int>\n" + body +
           "\n  end T;\n"
           "  public static async method Main(): Task\n"
           "    var s := 0;\n"
           "    for var i in 0..<100 do\n"
           "      s := s + await T(i);\n"
           "    end;\n"
           "    Console.WriteLine($\"{Res.Count} {s}\");\n"
           "  end Main;\n"
           "end Program;\n";
}

// 100 iterations under the strict AOT + ASan oracle: the program must run to
// completion and print "<destructor count> <sum>".
void expectClean(const std::string& src, const std::string& expected) {
    const JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// A compile-time rejection carrying the named diagnostic.
void expectRejected(const std::string& src, const char* needle) {
    const JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, needle));
}

}  // namespace

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_issue_repro_bang_arm_moves_the_selected_branch) {
    // The issue's repro: the `o!` arm is always selected.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var x: OC := (if i >= 0 then o! else OC.Empty);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_then_arm_selected_on_alternate_iterations) {
    // Per-path: moved on the even iterations, disposed by `o`'s own scope end
    // on the odd ones.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var x: OC := (if (i mod 2) = 0 then o! else OC.Empty);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_else_arm) {
    // The mirrored arm order.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var x: OC := (if (i mod 2) = 0 then OC.Empty else o!);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arm_in_an_assignment) {
    // An assignment is an owned-consuming position too.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var x: OC := OC.Empty;
    x := (if (i mod 2) = 0 then o! else OC.Empty);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arm_under_using_var) {
    // `using var` adopts the selected branch's value.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    using var x: OC := (if (i mod 2) = 0 then o! else OC.Empty);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arm_in_a_nested_conditional) {
    // The inner if-expression is itself a branch of the outer one.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var x: OC := (if i > 1000 then OC.Empty else (if (i mod 2) = 0 then o! else OC.Empty));
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arms_over_two_locals) {
    // Each local is moved on its own path and disposed on the other.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var p: OC? := OC.Text(new Res());
    var x: OC := (if (i mod 2) = 0 then o! else p!);
    return 1;)BZL"),
                "200 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_17_8_bang_over_the_whole_conditional) {
    // `(if c then o else OC.Empty)!`: the `!` wraps the construct.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var x: OC := (if (i mod 2) = 0 then o else OC.Empty)!;
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arm_stored_into_an_owned_field) {
    // The field holder: an owning slot takes the selected branch.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var h := new Hold();
    h.F := (if (i mod 2) = 0 then o! else OC.Empty);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arm_in_a_return) {
    // A `return` is an owned-consuming position.
    expectClean(prog(R"BZL(    var x := Mk2(i);
    return 1;)BZL", R"BZL(  public static method Mk2(i: int): OC
    var o: OC? := OC.Text(new Res());
    return (if (i mod 2) = 0 then o! else OC.Empty);
  end Mk2;
)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arm_into_a_nullable_target_with_a_null_arm) {
    // The nullable twin: a `null` arm absorbs (K7.5).
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var x: OC? := (if (i mod 2) = 0 then o! else null);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arm_with_an_inferred_binding_type) {
    // No declared type on the target.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var x := (if (i mod 2) = 0 then o! else OC.Empty);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_17_8_double_bang_arm) {
    // `o!!` peels to the same binding.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var x: OC := (if (i mod 2) = 0 then o!! else OC.Empty);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arm_beside_a_throw_arm) {
    // A `throw` arm absorbs (K7.5); the other arm still moves.
    expectClean(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var x: OC := (if i >= 0 then o! else throw new InvalidOperationException("x"));
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arm_over_a_generic_owning_choice) {
    // The generic face: `Opt<Res>`.
    expectClean(prog(R"BZL(    var o := Opt<Res>.Some(new Res());
    var x: Opt<Res> := (if (i mod 2) = 0 then o! else Opt<Res>.None);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arm_as_a_generic_class_constructor_argument) {
    // `new Box<OC>(...)`: the argument is an owned-consuming position and the
    // constructed object's field owns the selected value.
    expectClean(prog(R"BZL(    var o := OC.Text(new Res());
    using var b := new Box<OC>((if (i mod 2) = 0 then o! else OC.Empty));
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arm_in_an_async_body_after_an_await) {
    // The async twin: the local lives in the frame.
    expectClean(asyncProg(R"BZL(    var o := OC.Text(new Res());
    await Task.Delay(1);
    var x: OC := (if (i mod 2) = 0 then o! else OC.Empty);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_20_bang_arm_in_an_async_body_before_an_await) {
    // The maybe-consumed local is live across the `await`.
    expectClean(asyncProg(R"BZL(    var o := OC.Text(new Res());
    var x: OC := (if (i mod 2) = 0 then o! else OC.Empty);
    await Task.Delay(1);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_28_3_10_async_maybe_consumed_move_only_local_disposes_per_path) {
    // The statement form in an `async` body: rejected on the base with "would
    // be dropped here without disposal"; §28.3.10 makes the rule uniform across
    // body kinds, and the synchronous body already admitted it.
    expectClean(asyncProg(R"BZL(    var o := OC.Text(new Res());
    var x: OC := OC.Empty;
    if (i mod 2) = 0 then x := o; end if;
    await Task.Delay(1);
    return 1;)BZL"),
                "100 100\n");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_28_3_k7_4_issue_repro_read_after_a_bang_arm_is_rejected) {
    // The issue's second observation: `o` is maybe-consumed after the
    // construct.
    expectRejected(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var x: OC := (if i >= 0 then o! else OC.Empty);
    var y := o;
    return 1;)BZL"),
                   "may have already been consumed on another branch and may not be read here");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_28_3_k7_4_read_after_a_bare_arm_is_rejected) {
    // The bare-arm spelling joins the same way.
    expectRejected(prog(R"BZL(    var o: OC := OC.Text(new Res());
    var x: OC := (if (i mod 2) = 0 then o else OC.Empty);
    var y := o;
    return 1;)BZL"),
                   "may have already been consumed on another branch and may not be read here");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_28_3_k7_4_read_after_a_bang_arm_assignment_is_rejected) {
    // The assignment position.
    expectRejected(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var x: OC := OC.Empty;
    x := (if (i mod 2) = 0 then o! else OC.Empty);
    var y := o;
    return 1;)BZL"),
                   "may have already been consumed on another branch and may not be read here");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_28_3_k7_4_read_after_a_bang_arm_argument_is_rejected) {
    // The argument position (§15.3: by value-move).
    expectRejected(prog(R"BZL(    var o: OC? := OC.Text(new Res());
    var k := Sink.Take((if (i mod 2) = 0 then o! else OC.Empty));
    var y := o;
    return k;)BZL"),
                   "may have already been consumed on another branch and may not be read here");
}

BEZEL_TEST(ownership_owning_choice_conditional_arm_move_7890,
           spec_28_3_k7_4_read_after_a_bang_arm_in_an_async_body_is_rejected) {
    // The async twin of the read-after rejection.
    expectRejected(asyncProg(R"BZL(    var o := OC.Text(new Res());
    var x: OC := (if (i mod 2) = 0 then o! else OC.Empty);
    await Task.Delay(1);
    return (case o of Text(r) => r.N, _ => 0);)BZL"),
                   "may have already been consumed on another branch and may not be read here");
}
