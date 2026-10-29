// RETURNING a by-value-move PARAMETER of an OWNING-CHOICE type transfers the
// value to the caller: the callee must not ALSO dispose it at scope end
// (issue #7537; spec §15.3 *Return* / *Parameter passing*, §28.1 clause 1,
// §35.4, v0.350).
//
// THE GOVERNING TEXT (docs/Bezel_Language_Spec.md, v0.350).
//
//   §15.3 *Parameter passing* — "for a move-only V, it is by value-move
//   (transfer ownership)". §15.3 *Return* — returning a move-only value
//   transfers it to the caller. §28.1 clause 1 — `(owned x: V)` is identical to
//   plain `(x: V)` for a move-only V. §35.4 — an owning-classified choice is a
//   move-only value type.
//
// THE BUG (#7537). `lowerStmt`'s `return` arm (LlvmBackend_Stmts.cpp) excludes
// a returned binding from scope-end disposal through `retSlot`, which it looks
// up for a LocalSymbol only. A returned `owned` owning-choice PARAMETER kept
// the scope-end active-case pass that `registerUnwindOwnedParam` enrolled, so
// the callee freed the payload before `ret` and the caller then read and
// disposed the freed block. MEASURED at r30-c1 342da591 under
// `--sanitize=address` on the issue's repro (row 1): `AddressSanitizer:
// heap-use-after-free ... READ of size 8`.
//
// THE FIX zero-stores the parameter slot on the return path (the value is
// already read), so the null-guarded pass disposes nothing there while a
// sibling path that does NOT return the parameter still disposes it (row 3).
//
// THE ORACLE. A counting destructor; the count is printed after every
// iteration's result has been dropped, so a double free shows as an ASan exit
// and a leak as a LeakSanitizer exit or a short count (`runAotChecked`).
//
// per §15.3, §28.1, §35.4, v0.350

#include <string>

#include "support/aot_support.hpp"  // runAotChecked — the strict AOT + ASan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

const char* kDecls =
    "public class Res "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res._n := Res._n + 1; end destructor; "
    "end Res; "
    "public choice OChoice Text(owned r: Res), Empty; end OChoice; "
    "public choice G<T> Has(owned v: T), No; end G; ";

// `members` declares `Pass`; `body` is one loop iteration using `i`.
std::string prog(const std::string& members, const std::string& body,
                 int n = 200) {
    return std::string("module M; ") + kDecls +
           " public class Program " + members +
           " public static method Main() "
           "   var i := 0; "
           "   while i < " + std::to_string(n) + " do " + body +
           "     i := i + 1; "
           "   end while; "
           "   Console.WriteLine(Res._n); "
           " end Main; "
           "end Program;";
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

BEZEL_TEST(ownership_owned_choice_param_return_7537,
           spec_15_3_issue_repro_returned_owned_param_disposed_once_500x) {
    // The issue's repro (t_ret_ow.bzl), 500 iterations.
    expectRunOutput(
        runAotChecked(prog(
            "public static method Pass(owned src: OChoice): OChoice "
            "  return src; end Pass; ",
            "var osrc := new Res(); var src := OChoice.Text(move osrc); "
            "var z := Pass(move src); ",
            500)),
        std::string("500\n"));
}

BEZEL_TEST(ownership_owned_choice_param_return_7537,
           spec_15_3_return_move_of_owned_param_disposed_once) {
    expectRunOutput(
        runAotChecked(prog(
            "public static method Pass(owned src: OChoice): OChoice "
            "  return move src; end Pass; ",
            "var s := OChoice.Text(new Res()); var z := Pass(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_param_return_7537,
           spec_15_3_conditional_return_disposes_on_the_other_path) {
    // Path sensitivity: on the path that does NOT return `src` the callee
    // still owns it and disposes it at scope end; on the returning path the
    // caller's `z` disposes it. Exactly one disposal per iteration either way.
    expectRunOutput(
        runAotChecked(prog(
            "public static method Pass(owned src: OChoice, c: bool): OChoice "
            "  if c then return src; end if; "
            "  return OChoice.Empty; "
            "end Pass; ",
            "var s := OChoice.Text(new Res()); "
            "var z := Pass(move s, i mod 2 = 0); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_param_return_7537,
           spec_20_if_expression_return_moves_only_the_selected_branch) {
    // §20 *Ownership*: the selected branch of an if-expression in an
    // owned-consuming position moves; the unselected `src` stays owned by the
    // callee and is disposed at its scope end.
    expectRunOutput(
        runAotChecked(prog(
            "public static method Pass(owned src: OChoice, c: bool): OChoice "
            "  return (if c then src else OChoice.Empty); "
            "end Pass; ",
            "var s := OChoice.Text(new Res()); "
            "var z := Pass(move s, i mod 2 = 0); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_param_return_7537,
           spec_35_9_returned_nullable_owned_param_disposed_once) {
    expectRunOutput(
        runAotChecked(prog(
            "public static method Pass(owned src: OChoice?): OChoice? "
            "  return src; end Pass; ",
            "var s: OChoice? := OChoice.Text(new Res()); "
            "var z := Pass(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_param_return_7537,
           spec_17_8_returned_null_forgiven_owned_param_disposed_once) {
    // §17.8: `src!` is `src` — the same transfer as a bare `return src;`.
    expectRunOutput(
        runAotChecked(prog(
            "public static method Pass(owned src: OChoice?): OChoice "
            "  return src!; end Pass; ",
            "var s: OChoice? := OChoice.Text(new Res()); "
            "var z := Pass(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_param_return_7537,
           spec_15_3_instance_method_returned_owned_param_disposed_once) {
    expectRunOutput(
        runAotChecked(prog(
            "public constructor() end constructor; "
            "public method Pass(owned src: OChoice): OChoice "
            "  return src; end Pass; ",
            "var p := new Program(); var s := OChoice.Text(new Res()); "
            "var z := p.Pass(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_param_return_7537,
           spec_35_4_generic_choice_returned_owned_param_disposed_once) {
    // §3.6 rule 2 + §35.4: `G<Res>` classifies owning per instantiation.
    expectRunOutput(
        runAotChecked(prog(
            "public static method Pass(owned src: G<Res>): G<Res> "
            "  return src; end Pass; ",
            "var s := G<Res>.Has(new Res()); var z := Pass(move s); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_param_return_7537,
           spec_14_generic_method_returned_owned_param_disposed_once) {
    expectRunOutput(
        runAotChecked(prog(
            "public static method Pass<T>(owned src: OChoice, x: T): OChoice "
            "  return src; end Pass; ",
            "var s := OChoice.Text(new Res()); var z := Pass<int>(move s, 1); ")),
        std::string("200\n"));
}

BEZEL_TEST(ownership_owned_choice_param_return_7537,
           spec_31_2_return_through_finally_disposed_once) {
    // §31.2: a `return` inside `try … finally` routes through the finally.
    expectRunOutput(
        runAotChecked(prog(
            "public static method Pass(owned src: OChoice): OChoice "
            "  try return src; finally Res._n := Res._n + 0; end; "
            "end Pass; ",
            "var s := OChoice.Text(new Res()); var z := Pass(move s); ")),
        std::string("200\n"));
}
