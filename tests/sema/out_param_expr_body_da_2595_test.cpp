// §22.3 (v0.236) / fix #2595 — callee-side definite assignment of `out`
// parameters on an EXPRESSION-BODIED method (`=> E;`).
//
// Spec §22.3 (v0.236), verbatim: "The callee must assign every `out` parameter
// on every **normal-completion (return) path** (a path that exits by `throw`
// carries no assignment obligation); failing to do so is a compile error."
//
// Before the fix, `checkDefiniteAssignment` entered the walker only for
// `MethodBodyKind::Block` (DefiniteAssignment.cpp checkMember), so an
// expression-bodied method's `out` parameters were never seeded and the §22.3
// check never ran: `method Fill(out n: int): int => 5;` was ADMITTED and the
// caller's recipient was left reading unassigned storage.
//
// The fix walks the expression body as the single normal-completion return
// path it is. It is deliberately NOT a blanket rejection of `out` on
// expression-bodied methods: an expression body CAN discharge the obligation by
// passing the parameter on as an `out` argument (`=> Helper(out n)`), a shape
// that compiles and runs correctly today — so the accept-side tests below are
// the false-reject guard, and are as load-bearing as the reject-side ones.
//
// These assert on diagnostic TEXT, not a coarse "has errors" bit, so a
// rejection for some unrelated reason cannot masquerade as the §22.3 one.

#include <string>

#include "support/da_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckDA;

namespace {

// The §22.3 callee-side out-param diagnostic (MethodDA::checkOutParams).
bool hasOutParamError(const bezel::test::DAResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("out parameter") != std::string::npos &&
            d.message.find("not definitely assigned") != std::string::npos &&
            d.message.find("22.3") != std::string::npos)
            return true;
    return false;
}

// The definite-assignment read diagnostic (MethodDA::checkRead) — an `out`
// parameter is "freely readable AFTER assignment" (§22.3), so a read of one
// still unassigned is an error on top of the missing assignment.
bool hasUnassignedReadError(const bezel::test::DAResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("read of unassigned local") != std::string::npos)
            return true;
    return false;
}

bool hasAnyError(const bezel::test::DAResult& r) {
    return r.diagnostics.hasErrors();
}

}  // namespace

// ---------------------------------------------------------------------------
// Reject side — the defect #2595 reported.
// ---------------------------------------------------------------------------

// §22.3: the issue's literal repro, reduced to a non-generic class. The single
// return path returns `5` and never assigns `n`.
BEZEL_TEST(sema_out_expr_body_2595, expr_body_never_assigns_out_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method Fill(out n: int): int => 5; "
        "end Box;");
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// §22.3: the issue's literal GENERIC repro — `Box<K>`. The gap was found while
// verifying #2588 (codegen for ref/out on expression-bodied generic methods),
// so the generic shape is the one originally observed.
BEZEL_TEST(sema_out_expr_body_2595, generic_expr_body_never_assigns_out_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box<K> "
        "  public method Fill(out n: int): int => 5; "
        "end Box;");
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// §22.3: reading the `out` parameter in the expression body before it is
// assigned is BOTH a read-of-unassigned error and a missing-assignment error —
// exact parity with what the block form `return n + 1;` already produced.
BEZEL_TEST(sema_out_expr_body_2595, expr_body_reads_unassigned_out_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method Read(out n: int): int => n + 1; "
        "end Box;");
    BEZEL_EXPECT_TRUE(hasUnassignedReadError(r));
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// §22.3: a `static` expression-bodied method is checked the same way — the
// obligation is on the callee's return path, not on the receiver.
BEZEL_TEST(sema_out_expr_body_2595, static_expr_body_never_assigns_out_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public static method Fill(out n: int): int => 5; "
        "end Box;");
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// §22.3: only the SECOND of two `out` parameters is discharged — the first is
// still unassigned on the return path, so the method is rejected. Guards the
// per-parameter (not "any out param assigned") granularity of the check.
BEZEL_TEST(sema_out_expr_body_2595, expr_body_assigns_only_one_of_two_outs_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method Helper(out k: int): int "
        "    k := 7; "
        "    return 1; "
        "  end Helper; "
        "  public method Fill(out a: int, out b: int): int => Helper(out b); "
        "end Box;");
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// ---------------------------------------------------------------------------
// Accept side — the false-reject guard. A widened rejection that broke these
// would be worse than the bug it fixes.
// ---------------------------------------------------------------------------

// §22.3: an expression body DOES discharge the obligation when it passes the
// parameter on as an `out` argument — within the callee an `out` parameter
// "denotes a callee-local slot", an assignable storage location. This shape
// compiles AND runs correctly (verified end-to-end: the caller observes 7), so
// the blanket "reject `out` on expression-bodied methods" reading that #2595
// suggested would have been a false reject on working code.
BEZEL_TEST(sema_out_expr_body_2595, expr_body_assigns_through_out_arg_accepted_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method Helper(out k: int): int "
        "    k := 7; "
        "    return 1; "
        "  end Helper; "
        "  public method Fill(out n: int): int => Helper(out n); "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §22.3: both `out` parameters discharged through two calls in one expression.
BEZEL_TEST(sema_out_expr_body_2595, expr_body_assigns_through_both_outs_accepted_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method Helper(out k: int): int "
        "    k := 7; "
        "    return 1; "
        "  end Helper; "
        "  public method Fill(out a: int, out b: int): int => "
        "    Helper(out a) + Helper(out b); "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §22.3: an ordinary expression-bodied method — no `out` parameter at all — is
// untouched. This is the overwhelmingly common shape in the language; the walk
// is not even entered for it.
BEZEL_TEST(sema_out_expr_body_2595, expr_body_without_out_param_accepted) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method Twice(v: int): int => v + v; "
        "  public method Name(): string => \"box\"; "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §22.3: a `ref` parameter carries NO callee-side assignment obligation ("the
// variable must be initialized before the call" — the caller's obligation), so
// an expression body that merely reads it stays legal. This is the shape #2588
// landed codegen for; regressing it would break that fix's tests.
BEZEL_TEST(sema_out_expr_body_2595, expr_body_ref_param_read_only_accepted_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method Bump(ref n: int): int => n + 1; "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §22.3 / #2588: the generic-typed `ref` parameter on an expression-bodied
// generic method — `Read(ref x: K): K => x`, verbatim from the #2588 test file.
BEZEL_TEST(sema_out_expr_body_2595, generic_expr_body_ref_param_accepted_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box<K> "
        "  public method Read(ref x: K): K => x; "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §22.3: an ABSTRACT method declaration (`;` body, no implementation) has no
// return path to discharge the obligation on — the walk must not be entered for
// it. An interface method is the same shape.
BEZEL_TEST(sema_out_expr_body_2595, abstract_out_method_not_checked_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "public interface IParser "
        "  method TryParse(s: string, out n: int): bool; "
        "end IParser;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
}

// ---------------------------------------------------------------------------
// Accept side, second wave — the shapes a first draft of this fix FALSELY
// REJECTED. Each was caught by review and is now pinned. All four compile,
// link, and run correctly (verified end-to-end).
// ---------------------------------------------------------------------------

// §17.12 / §22.3: `throw` is an EXPRESSION in Bezel — "Valid in any expression
// position" — and §22.3 exempts it: "a path that exits by `throw` carries no
// assignment obligation". So `=> throw new NotImplementedException();`, the
// idiomatic unimplemented stub, has NO normal-completion path and owes nothing.
// The block-bodied equivalent (a `throw` statement) was always accepted; a
// first draft of this fix rejected the expression form, inverting exactly the
// parity it was written to establish.
BEZEL_TEST(sema_out_expr_body_2595, expr_body_throw_accepted_spec_17_12_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method TryParse(s: string, out n: int): bool => "
        "    throw new NotImplementedException(); "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
}

// §22.3 parity anchor for the above: the BLOCK form of the same stub. It was
// accepted before this fix and must stay accepted — the two forms must agree.
BEZEL_TEST(sema_out_expr_body_2595, block_body_throw_accepted_spec_17_12_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method TryParse(s: string, out n: int): bool "
        "    throw new NotImplementedException(); "
        "  end TryParse; "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
}

// §22.3: the obligation discharged inside a `(case … )` expression body, every
// arm assigning. `CaseExpr` has no arm in the DA walker, so the `Helper(out n)`
// calls are invisible to it — reporting there would be a false reject. The walk
// records that it could not see the form and declines to report. Runs correctly
// (the caller observes 7).
BEZEL_TEST(sema_out_expr_body_2595, expr_body_case_form_not_falsely_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method Helper(out k: int): int "
        "    k := 7; "
        "    return 1; "
        "  end Helper; "
        "  public method Fill(sel: int, out n: int): int => (case sel of "
        "    0 => Helper(out n), "
        "    _ => Helper(out n)); "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
}

// §22.3 / §18.5: same, for an interpolated string body — the hole's expression
// is likewise invisible to the walker (`InterpolatedStringExpr` has no arm).
// Runs correctly (the caller observes 7).
BEZEL_TEST(sema_out_expr_body_2595, expr_body_interpolation_not_falsely_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method Helper(out k: int): int "
        "    k := 7; "
        "    return 1; "
        "  end Helper; "
        "  public method Fill(out n: int): string => $\"{Helper(out n)}\"; "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
}

// §22.2: "A method with **no declared return type** may also use a single
// *assignment* as its arrow body: `public method SetTotal(v: int) => _total :=
// v;`". That form CAN discharge the obligation directly, and does — this
// compiles and the caller observes 5. Pins the one arrow-body shape that
// assigns an `out` parameter without any call at all.
BEZEL_TEST(sema_out_expr_body_2595, assignment_arrow_body_assigns_out_accepted_spec_22_2) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method Fill(out n: int) => n := 5; "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §22.3: `out owned` on an expression body is REJECTED, and correctly so — but
// for a subtler reason than "an expression cannot assign". §22.3 restricts an
// `out owned` CALL SITE to the inline forms `out using var x` / `out owned var
// x`, and an inline form declares a FRESH variable, so the parameter can never
// be forwarded to another `out owned` position. Combined with §22.2's
// assignment arrow body being available only to a method with NO declared
// return type, the obligation is undischargeable from an `out owned`
// expression body. Pinned because the rejection currently arrives from this
// §22.3 walk, while the call-site form rule is enforced elsewhere — a refactor
// of either mechanism must not silently drop the other.
BEZEL_TEST(sema_out_expr_body_2595, expr_body_out_owned_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Conn "
        "  public constructor() end constructor; "
        "end Conn; "
        "class Box "
        "  public method Make(out owned c: Conn): bool => true; "
        "end Box;");
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// ---------------------------------------------------------------------------
// Parity anchor — the block form's behavior must be unchanged by the shared
// `seedOutParams` refactor the fix introduced.
// ---------------------------------------------------------------------------

// §22.3: a BLOCK-bodied method that never assigns its `out` parameter was
// already rejected before the fix and must stay rejected after it.
BEZEL_TEST(sema_out_expr_body_2595, block_body_never_assigns_out_still_rejected_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method Fill(out n: int): int "
        "    return 5; "
        "  end Fill; "
        "end Box;");
    BEZEL_EXPECT_TRUE(hasOutParamError(r));
}

// §22.3: a block-bodied method that DOES assign its `out` parameter was already
// accepted and must stay accepted.
BEZEL_TEST(sema_out_expr_body_2595, block_body_assigns_out_still_accepted_spec_22_3) {
    auto r = parseAndCheckDA(
        "module M; "
        "class Box "
        "  public method Fill(out n: int): int "
        "    n := 5; "
        "    return n; "
        "  end Fill; "
        "end Box;");
    BEZEL_EXPECT_FALSE(hasOutParamError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}
