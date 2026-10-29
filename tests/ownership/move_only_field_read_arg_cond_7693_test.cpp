// The #6233 argument recorders (a plain read of a MOVE-ONLY FIELD passed to a
// by-value-move parameter, and an `owned`-modified field read passed to an
// `owned` parameter) must run at EVERY expression position, not only at the
// binding / assignment / bare-call positions the dataflow routes through them
// (issue #7693; see also #6233, #7541; spec §15.3 *Parameter passing* and
// *Whole-value reassignment*, K1.5, §28.8.1, v0.350).
//
// THE GOVERNING TEXT. Identical to move_only_field_read_arg_6233_test.cpp:
//
//   §15.3 *Parameter passing* (v0.350): "for a move-only V, it is by value-move
//   (transfer ownership)". §15.3 *Whole-value reassignment*: "a move-only
//   field is never moved-from (Appendix C.3) — so at a field the plain read
//   `_arr` lends (K1.5): … `_arr := f(_arr);` is rejected at the argument, a
//   by-value-move parameter requiring the spelled transfer
//   `f(move _arr with fresh)`." Nothing in §15.3 makes the verdict depend on
//   where the call sits, so a call inside an `if` / `elif` / `while`
//   condition, a `case` scrutinee or guard, a `for` iterable, a `throw`
//   operand, an expression body, a lambda body, or a call through a
//   FUNCTION-TYPED value (whose own parameter list is the formal list) is
//   rejected exactly like the same call as a statement.
//
// THE DEFECT. `Count(bx.V) > 0` in a condition (a BinaryExpr root) reached no
// recorder, so the field was lent to a parameter that disposes it while `Bx`
// disposes it again: an AddressSanitizer failure at runtime (measured on
// origin/main). A lambda body is not dataflow-analysed at all, and a function-
// typed callee had no formal list to classify against.
//
// THE ORACLE. Each reject row asserts the specific diagnostic and a real
// source location; each accept row runs 100 iterations and checks the exact
// disposal count, so a legal neighbour that started leaking, double-disposing
// or being over-rejected fails too.
//
// per §15.3, §28.8.1, K1.5, v0.350

#include <string>

#include "support/aot_support.hpp"  // runAot — the strict AOT oracle
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// `bxExtra` adds members to `Bx`, `progExtra` to `Program`: a reject row's
// offending method must not leak into any other row's program.
std::string decls(const std::string& bxExtra) {
    return std::string(
    "class Res "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res._n := Res._n + 1; end destructor; "
    "end Res; "
    "choice OChoice Text(owned r: Res), Empty; end OChoice; "
    "choice VC A(x: int), B; end VC; "
    "choice Wrap W(inner: OChoice), None; end Wrap; "
    "class Bx "
    "  public owned V: OChoice := OChoice.Empty; "
    "  public owned NV: OChoice? := null; "
    "  public A: [2 owned]Res?; "
    "  public C: VC := VC.B; "
    "  public Cls: Res? := null; "
    "  public constructor() "
    "    V := OChoice.Text(new Res()); NV := OChoice.Text(new Res()); "
    "    A := [new Res(), null]; "
    "  end constructor; "
    "  public method Recv(src: OChoice) using var k := src; end Recv; ") +
    bxExtra + std::string(
    "end Bx; "
    "class Outer "
    "  public owned In: Bx := new Bx(); "
    "  public constructor() end constructor; "
    "end Outer; "
    "class Ct "
    "  public constructor(v: OChoice) using var k := v; end constructor; "
    "end Ct; ");
}

const char* kSinks =
    "public static method Keep(src: OChoice) using var k := src; end Keep; "
    "public static method KeepO(owned src: OChoice) using var k := src; end KeepO; "
    "public static method KeepN(src: OChoice?) using var k := src; end KeepN; "
    "public static method KeepA(src: [2 owned]Res?) end KeepA; "
    "public static method Count(src: OChoice): int using var k := src; return 1; end Count; "
    "public static method Two(n: int, src: OChoice) using var k := src; end Two; "
    "public static method G<T>(x: T) end G; "
    "public static method PeekVC(v: VC) end PeekVC; "
    "public static method PeekR(r: Res?) end PeekR; "
    "public static method Make(): OChoice return OChoice.Text(new Res()); end Make; ";

// A program whose `Run()` body is `body`. Reject rows never run it.
std::string rejectProg(const std::string& body,
                       const std::string& bxExtra = "",
                       const std::string& progExtra = "") {
    return std::string("module M; ") + decls(bxExtra) + " class Program " +
           kSinks + progExtra + " public static method Run() " + body + " end Run; "
           " public static method Main() Run(); end Main; end Program;";
}

// A program running `body` 100 times and printing the disposal count.
std::string loopProg(const std::string& body,
                     const std::string& progExtra = "") {
    return std::string("module M; ") + decls("") + " class Program " + kSinks +
           progExtra + " public static method Body() " + body + " end Body; "
           " public static method Main() "
           "   var i := 0; "
           "   while i < 100 do Body(); i := i + 1; end while; "
           "   Console.WriteLine(Res._n); "
           " end Main; end Program;";
}

void expectFieldArgRejected(const std::string& body, const char* field,
                            const std::string& bxExtra = "",
                            const std::string& progExtra = "") {
    auto r = runAot(rejectProg(body, bxExtra, progExtra));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    const std::string needle = std::string("cannot pass the field `") + field +
                               "` to a parameter that takes ownership";
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle.c_str()));
}

void expectRunCount(const std::string& body, const std::string& count,
                    const std::string& progExtra = "") {
    auto r = runAot(loopProg(body, progExtra));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, count + "\n");
}


// Extra sinks used only by this file's rows.
const char* kMore =
    "public static method CountO(owned src: OChoice): int "
    "  using var k := src; return 1; end CountO; "
    "public static method Pk(c: VC): int => 1; ";

void expectOwnedSinkBorrowRejected(const std::string& body,
                                   const std::string& bxExtra = "") {
    auto r = runAot(rejectProg(body, bxExtra, kMore));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

}  // namespace

// Reject rows: the per-position holes (#7693).

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_if_condition_rejected) {
    expectFieldArgRejected("var bx := new Bx(); if Count(bx.V) > 0 then Console.WriteLine(1); end if;", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_elif_condition_rejected) {
    expectFieldArgRejected("var bx := new Bx(); if false then Console.WriteLine(1); elif Count(bx.V) > 0 then Console.WriteLine(2); end if;", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_while_condition_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var j := 0; while Count(bx.V) < 0 and j < 1 do j := j + 1; end while;", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_negated_condition_rejected) {
    expectFieldArgRejected("var bx := new Bx(); if not (Count(bx.V) > 0) then Console.WriteLine(1); end if;", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_conditional_expression_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var q := (if Count(bx.V) > 0 then 1 else 2);", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_case_guard_rejected) {
    expectFieldArgRejected("var bx := new Bx(); case 1 of var z when Count(bx.V) > 0 => Console.WriteLine(1), _ => Console.WriteLine(2) end case;", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_case_scrutinee_rejected) {
    expectFieldArgRejected("var bx := new Bx(); case Count(bx.V) of 1 => Console.WriteLine(1), _ => Console.WriteLine(2) end case;", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_for_iterable_rejected) {
    expectFieldArgRejected("var bx := new Bx(); for var e in Mk(Count(bx.V)) do Console.WriteLine(e); end for;", "V", "", "public static method Mk(n: int): [3]int => [1,2,3]; ");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_throw_operand_rejected) {
    expectFieldArgRejected("var bx := new Bx(); try throw new InvalidOperationException($\"{Count(bx.V)}\"); catch InvalidOperationException => Console.WriteLine(1) end try;", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_expression_bodied_member_rejected) {
    expectFieldArgRejected("var bb := new Bx(); var q := R(bb);", "V", "", "public static method R(bx: Bx): int => Count(bx.V); ");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_binary_rooted_return_rejected) {
    expectFieldArgRejected("var bb := new Bx(); var q := R(bb);", "V", "", "public static method R(bx: Bx): int return Count(bx.V) + 1; end R; ");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_call_receiver_chain_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var q := Count(bx.V).ToString();", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_index_expression_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var arr := [1,2]; var q := arr[Count(bx.V)];", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_member_condition_of_own_method_rejected) {
    expectFieldArgRejected("var bx := new Bx(); bx.Cond();", "V", "public method Cond() if Program.Count(V) > 0 then Console.WriteLine(1); end if; end Cond; ", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_expression_bodied_property_rejected) {
    expectFieldArgRejected(
        "var bx := new Bx(); var q := bx.Prp;", "V",
        "public property Prp: int => Program.Count(V); ");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_block_property_getter_rejected) {
    expectFieldArgRejected(
        "var bx := new Bx(); var q := bx.Prp;", "V",
        "public property Prp: int get => do return Program.Count(V); end; "
        "end Prp; ");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_lambda_expression_body_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var f := lambda() => Count(bx.V);", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_lambda_block_body_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var f := lambda() => do var q := Count(bx.V); end;", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_lambda_block_statement_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var f := lambda() => do Keep(bx.V); end;", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_nested_lambda_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var f := lambda() => lambda() => Count(bx.V);", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_in_lambda_condition_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var f := lambda() => do if Count(bx.V) > 0 then Console.WriteLine(1); end if; end;", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_constructor_argument_in_condition_rejected) {
    expectFieldArgRejected("var bx := new Bx(); if new Ct(bx.V) is Ct then Console.WriteLine(1); end if;", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_constructor_argument_in_lambda_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var f := lambda() => new Ct(bx.V);", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_to_function_typed_local_callee_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var g := lambda(o: OChoice) => 1; var q := g(bx.V);", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_to_function_typed_parameter_callee_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var q := Ap(lambda(o: OChoice) => 1, bx);", "V", "", "public static method Ap(g: (OChoice) -> int, b: Bx): int => g(b.V); ");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_to_function_typed_callee_in_condition_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var g := lambda(o: OChoice) => 1; if g(bx.V) > 0 then Console.WriteLine(1); end if;", "V", "", "");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_field_to_function_typed_callee_in_lambda_rejected) {
    expectFieldArgRejected("var bx := new Bx(); var g := lambda(o: OChoice) => 1; var f := lambda() => g(bx.V);", "V", "", "");
}

// Owned-formal spelling keeps the §28.8 borrow-into-owned-sink error.

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_owned_spelling_field_in_if_condition_keeps_borrow_error) {
    expectOwnedSinkBorrowRejected("var bx := new Bx(); if CountO(bx.V) > 0 then Console.WriteLine(1); end if;");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_owned_spelling_field_in_while_condition_keeps_borrow_error) {
    expectOwnedSinkBorrowRejected("var bx := new Bx(); var j := 0; while CountO(bx.V) < 0 do j := j + 1; end while;");
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_owned_spelling_field_in_lambda_keeps_borrow_error) {
    expectOwnedSinkBorrowRejected("var bx := new Bx(); var f := lambda() => CountO(bx.V);");
}

// Accept rows: legal neighbours, each disposing exactly once.

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_copyable_field_in_conditions_accepted) {
    expectRunCount("var bx := new Bx(); var n := 0; if Pk(bx.C) > 0 then n := n + 1; end if; while Pk(bx.C) < 0 do n := n + 1; end while; G(n);", "300", kMore);
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_equals_borrow_in_condition_accepted) {
    expectRunCount("var bx := new Bx(); var n := 0; if bx.A.Equals(bx.A) then n := n + 1; end if; G(n);", "300", kMore);
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_copyable_field_in_lambda_body_accepted) {
    expectRunCount("var bx := new Bx(); var f := lambda() => do var q := Pk(bx.C); end; f();", "300", kMore);
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_capture_only_lambda_accepted) {
    expectRunCount("var bx := new Bx(); var f := lambda() => bx.C; var c := f(); G(c);", "300", kMore);
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_copyable_param_function_typed_callee_accepted) {
    expectRunCount("var bx := new Bx(); var g := lambda(o: VC) => 1; var n := 0; if g(bx.C) > 0 then n := n + 1; end if; G(n);", "300", kMore);
}

BEZEL_TEST(move_only_field_read_arg_cond_7693,
           spec_15_3_moved_local_to_owned_param_in_condition_accepted) {
    expectRunCount("var o := Make(); var n := 0; if CountO(move o) > 0 then n := n + 1; end if; G(n);", "100", kMore);
}
