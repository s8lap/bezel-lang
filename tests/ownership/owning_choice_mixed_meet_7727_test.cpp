// The K7 meet over an OWNING-CHOICE-typed `if` / `case` / `??` rejects a
// read of storage beside an owned arm (issue #7727; spec §28.3 K7.3, §28.3.4
// producers table, §20 *Ownership*, §35.4, v0.357).
//
// THE GOVERNING TEXT (docs/Bezel_Language_Spec.md, v0.357).
//
//   §28.3.4 producers table — "Field access `obj.f` where `f` is `owned` |
//   Borrowed (source: `obj`)"; the `a ?? b` row is the same K7 meet. (No
//   property row here: §23.5 does not admit a property whose declared type
//   is a move-only value type.)
//
//   K7.3 — "var d := (if flag then c else new Widget("d")); // error (K7.3):
//   borrowed ⊓ owned".
//
//   §20 *Ownership* — for branches of a move-only value type: "in an
//   owned-consuming position — a binding, an assignment, a plain or `owned`
//   argument, a `return`, an owning slot — the **selected** branch's value
//   moves ...; in a borrowing position — an operand, a receiver, an index —
//   every branch is a borrow of its binding. ... A move-only value is never
//   bound by a borrow arm-meet".
//
// THE BUG (#7727). The meet checks counted a borrow arm only when it was
// class-, interface- or array-typed, so an owning-choice meet never had one:
// `using var x := (if c then h.F else OC.Text(new Res()))` compiled, `x` took
// the field's bits on one path, and the scope-end pass disposed a payload `h`
// still owns. MEASURED at r30-c1 46219d9ba under ASan on the first row:
// `heap-use-after-free`; the issue's comment adds the move-only LOCAL arm
// `(if c then h.F else o)` and `h.G ?? o`, same report.
//
// What is an owned arm. A fresh production (a case construction, a call, the
// payload-free case `OC.Empty`) is owned in every position. A bare move-only
// BINDING arm is owned only at an owned-consuming position, where §20 has it
// move; in a borrowing position it is a borrow and the set is not mixed.
//
// Not decided here: a meet whose arms are ALL reads of storage, and the
// direct read `var x: OC := h.F`. Both wait on the open question of what a
// plain read of a move-only field binds (issues #7612, #6233).
//
// Every row below fails at r30-c1 46219d9ba: the program compiled there.
//
// per §28.3, §28.3.4, §20, §35.4, §15.3, v0.357

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
public class H
  public owned F: OC := OC.Text(new Res());
  public owned G: OC? := OC.Text(new Res());
  public owned FG: Opt<Res> := Opt<Res>.Some(new Res());
  public constructor() end constructor;
end H;
public class Sink
  public static method Take(owned v: OC): int => 1;
  public static method TakeP(v: OC): int => 1;
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

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_issue_repro_field_arm_beside_a_fresh_arm_under_using_var) {
    // The issue's repro.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    using var x := (if c then h.F else OC.Text(new Res()));
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_fresh_arm_first) {
    // The mirrored arm order.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    using var x := (if c then OC.Text(new Res()) else h.F);
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_field_arm_beside_a_fresh_arm_under_plain_var) {
    // The diagnosis is binding-form-independent.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var x: OC := (if c then h.F else OC.Text(new Res()));
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_20_issue_comment_field_arm_beside_a_moving_local_arm_under_using_var) {
    // The issue's comment: at a binding the LOCAL arm moves (§20), so the set
    // is storage ⊓ owned.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var o := OC.Text(new Res());
    using var x := (if c then h.F else o);
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_20_field_arm_beside_a_moving_local_arm_under_plain_var) {
    // Same, plain `var`.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var o := OC.Text(new Res());
    var x: OC := (if c then h.F else o);
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_issue_comment_coalesce_of_a_field_and_a_moving_local) {
    // The issue's comment: `h.G ?? o`.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var o := OC.Text(new Res());
    using var x := h.G ?? o;
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_coalesce_of_a_field_and_a_moving_local_under_plain_var) {
    // Same, plain `var`.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var o := OC.Text(new Res());
    var x: OC := h.G ?? o;
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_field_arm_beside_a_payload_free_case) {
    // `OC.Empty` is a fresh value: the new holder would own it on one path and
    // the field's bits on the other.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    using var x := (if c then h.F else OC.Empty);
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_coalesce_of_a_field_and_a_payload_free_case) {
    // The `??` spelling of the same pair.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    using var x := h.G ?? OC.Empty;
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_case_expression_arms) {
    // §20: "The same reading governs an expression-`case` arm".
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    using var x := (case i mod 2 of 0 => h.F, _ => OC.Text(new Res()));
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_field_arm_beside_a_fresh_arm_in_an_assignment) {
    // The assignment position.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var x: OC := OC.Empty;
    x := (if c then h.F else OC.Text(new Res()));
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_field_arm_beside_a_fresh_arm_as_an_owned_argument) {
    // The argument position (the base leaked the fresh arm here).
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var k := Sink.Take((if c then h.F else OC.Text(new Res())));
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_field_arm_beside_a_fresh_arm_in_a_borrowing_position) {
    // A fresh arm is owned in EVERY position, so a scrutinee is mixed as well
    // (the base leaked the fresh arm here).
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var k := (case (if c then h.F else OC.Text(new Res())) of Text(r) => r.N, _ => 0);
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_field_arm_beside_a_fresh_arm_in_a_return) {
    // The `return` position.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    using var x := Pick(h, c);
    return 1;)BZL", R"BZL(  public static method Pick(h: H, c: bool): OC
    return (if c then h.F else OC.Text(new Res()));
  end Pick;
)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_20_field_arm_beside_a_moving_local_arm_in_a_return) {
    // The local arm moves at a `return` (§15.3 *Return*).
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    using var x := Pick(h, c);
    return 1;)BZL", R"BZL(  public static method Pick(h: H, c: bool): OC
    var o := OC.Text(new Res());
    return (if c then h.F else o);
  end Pick;
)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_20_field_arm_beside_a_moving_local_arm_in_an_assignment) {
    // The local arm moves at an assignment.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var o := OC.Text(new Res());
    var x: OC := OC.Empty;
    x := (if c then h.F else o);
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_20_field_arm_beside_a_moving_local_arm_as_an_owned_argument) {
    // §20 names "a plain or `owned` argument" an owned-consuming position.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var o := OC.Text(new Res());
    var k := Sink.Take((if c then h.F else o));
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_20_field_arm_beside_a_moving_local_arm_as_a_plain_argument) {
    // The plain formal of a move-only type is by value-move (§15.3).
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var o := OC.Text(new Res());
    var k := Sink.TakeP((if c then h.F else o));
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_17_8_bang_over_a_nullable_field_arm) {
    // `h.G!` is still a read of the field.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    using var x := (if c then h.G! else OC.Text(new Res()));
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_null_conditional_field_arm) {
    // `hn?.G` reads the same storage.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var hn: H? := h;
    using var x: OC? := (if c then hn?.G else OC.Text(new Res()));
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_nullable_field_arm_into_a_nullable_target) {
    // The nullable twin.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    using var x: OC? := (if c then h.G else OC.Text(new Res()));
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_generic_owning_choice_field_arm) {
    // The generic face: an `Opt<Res>` field.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    using var x := (if c then h.FG else Opt<Res>.Some(new Res()));
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_implicit_this_field_arm_beside_a_fresh_arm) {
    // Inside a method the bare name `PF` is the read `this.PF`.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var p := new Program();
    var k := p.PickIf(c);
    return 1;)BZL", R"BZL(  public owned PF: OC := OC.Text(new Res());
  public constructor() end constructor;
  public method PickIf(c: bool): int
    using var x := (if c then PF else OC.Text(new Res()));
    return 1;
  end PickIf;
)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_implicit_this_nullable_field_coalesce_beside_a_payload_free_case) {
    // The `??` spelling over the bare field name.
    expectRejected(prog(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    var p := new Program();
    var k := p.PickQ();
    return 1;)BZL", R"BZL(  public owned PG: OC? := OC.Text(new Res());
  public constructor() end constructor;
  public method PickQ(): int
    using var x := PG ?? OC.Empty;
    return 1;
  end PickQ;
)BZL"),
                   "mix an owned move-only value with a read of storage");
}

BEZEL_TEST(ownership_owning_choice_mixed_meet_7727,
           spec_28_3_k7_3_field_arm_beside_a_fresh_arm_in_an_async_body) {
    // The async twin.
    expectRejected(asyncProg(R"BZL(    var c := (i mod 2) = 0;
    var h := new H();
    using var x := (if c then h.F else OC.Text(new Res()));
    await Task.Delay(1);
    return 1;)BZL"),
                   "mix an owned move-only value with a read of storage");
}
