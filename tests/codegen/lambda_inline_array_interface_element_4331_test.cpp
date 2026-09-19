// fix #4331 / spec §15.1 *Inline arrays* / §8.6 *Boxing* / §27.5
// *Closures* (v0.298). An expression-bodied lambda whose body is an inline
// array collection literal returning `[N]IHolder` via `() -> [N]IHolder` SIGSEGVd
// at runtime even for a CONFORMING CLASS element (not just a record).
//
// ROOT CAUSE (traced, LlvmBackend_Exprs2.cpp). `lowerLambda` only called
// `materializeHeapCollectionLiteral` for collection-literal expression bodies,
// but that returns null for inline arrays (they are not heap collections).
// `lowerExpr` also returns null for `CollectionLiteralExpr`, so `v` stayed
// null and the lambda emitted a null/zero inline array return — a
// `ret zeroinitializer` of the `[1 x %box.IHolder]` type. The caller then
// received an all-zero inline array, `outer[0]` read a null box pointer, and
// `.GetV()` dispatched through a null itable → SIGSEGV.
//
// THE FIX adds inline-array materialization to `lowerLambda`'s expression-body
// path, mirroring the existing `lowerReturnStmt` (LlvmBackend_Stmts.cpp) and
// `lowerLocalDecl` (LlvmBackend_Setup.cpp) inline-array paths: builds the
// inline array on the stack and returns it by value. The `boxForTarget` call
// (§8.6) is also included here — a value-typed (record) element needs boxing
// into its box representation before storing into an interface-typed slot,
// identical to the #4317 (#4317's Stmts.cpp sites) and #4335 (#4335's
// lowerLocalDecl site) fixes.
//
// BOUNDARY: block-bodied lambdas returning an inline array (`lambda() => do
// return [...]; end`) route through `lowerScopedBlock` → `lowerReturnStmt`,
// which has its own inline-array handling WITHOUT `boxForTarget` (#4317's
// fix target). That path is out of scope for this lane; this lane fixes only
// the expression-bodied lambda path.
//
// SCOPE: intentionally narrow — this lane fixes only the expression-bodied
// lambda inline-array return path. #4317/#4335 are separate lanes targeting
// the three `lowerReturnStmt` sites and the `lowerLocalDecl` site
// respectively.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// `IHolder` (one method), a genuinely conforming record `RecHolder` (§9.6 —
// a record may implement an interface declaring no settable member), and a
// genuinely conforming class `Widget` (the class element is already a bare
// pointer — no boxing needed, but the inline-array materialization IS needed
// to avoid the null-array SIGSEGV).
const std::string kPrelude =
    "module M; "
    "interface IHolder method GetV(): string; end IHolder; "
    "record RecHolder(V: string): IHolder "
    "  public method GetV(): string => V; "
    "end RecHolder; "
    "class Widget: IHolder "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public method GetV(): string => _s; "
    "  destructor end destructor; "
    "end Widget; ";

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── PRIMARY: the issue's verbatim repro — expression-bodied lambda returning
// [1 owned]IHolder with a CLASS element. Pre-fix: SIGSEGV (exit 139). Post-fix:
// returns 5 ("hello".Length).
// per §15.1 / §8.6 / §27.5 (issue #4331)
// NOTE: switched to `[1 owned]` (the spec-conformant form per §15.1.1 / #4336 /
// `569a680`): the non-owning `[1]IHolder` is a BORROW slot per §15.1 and the
// `new Widget(s)` initializer is now rejected by #4351's sema fix. The owning
// form takes ownership of the fresh widget on construction and exercises the
// SAME lambda expression-body materialization path this test pins.
BEZEL_TEST(lambda_inline_array_interface_element_4331,
           class_element_expr_bodied_lambda_issue_repro_spec_15_1) {
    BEZEL_EXPECT_EQ(run(
        kPrelude +
        "class C "
        "  public static method Main(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var f: () -> [1 owned]IHolder := lambda() => [new Widget(s)]; "
        "    var outer := f(); "
        "    return outer[0].GetV().Length; "
        "  end Main; "
        "end C;"),
        5);
}

// ── Record element variant — mirrors #4317/#4335's record case. The record
// element needs BOXING (§8.6) into its box representation before storing into
// the interface-typed slot. Pre-fix: SIGSEGV (exit 139, the same null-array
// path as the class case). Post-fix: returns 5.
// per §15.1 / §8.6 (issue #4331)
// See note above: switched to `[1 owned]` for #4351 (sema rejects `new ...`
// at a non-owning interface slot per §15.1).
BEZEL_TEST(lambda_inline_array_interface_element_4331,
           record_element_expr_bodied_lambda_boxed_spec_15_1_8_6) {
    BEZEL_EXPECT_EQ(run(
        kPrelude +
        "class C "
        "  public static method Main(): int "
        "    var f: () -> [1 owned]IHolder := lambda() => [new RecHolder(\"hello\")]; "
        "    var outer := f(); "
        "    return outer[0].GetV().Length; "
        "  end Main; "
        "end C;"),
        5);
}

// ── CONTROL: class element at LocalDecl (no lambda) still works — verifies
// this lane does NOT regress the #4310/#4317 working path.
// per §15.1 (issue #4331, control)
// See note above: switched to `[1 owned]` for #4351 (sema rejects `new ...`
// at a non-owning interface slot per §15.1).
BEZEL_TEST(lambda_inline_array_interface_element_4331,
           class_element_local_decl_unaffected_control_spec_15_1) {
    BEZEL_EXPECT_EQ(run(
        kPrelude +
        "class C "
        "  public static method Main(): int "
        "    var s := \"he\" + \"llo\"; "
        "    var outer: [1 owned]IHolder := [new Widget(s)]; "
        "    return outer[0].GetV().Length; "
        "  end Main; "
        "end C;"),
        5);
}

// ── CONTROL: multi-element inline array with class element — verifies the
// per-element loop works for more than one element.
// per §15.1 (issue #4331, control)
// See note above: switched to `[2 owned]` for #4351.
BEZEL_TEST(lambda_inline_array_interface_element_4331,
           class_element_multi_elem_expr_bodied_lambda_spec_15_1) {
    BEZEL_EXPECT_EQ(run(
        kPrelude +
        "class C "
        "  public static method Main(): int "
        "    var f: () -> [2 owned]IHolder := lambda() => [new Widget(\"hi\"), new Widget(\"!\")]; "
        "    var outer := f(); "
        "    return outer[0].GetV().Length + outer[1].GetV().Length; "
        "  end Main; "
        "end C;"),
        3);  // "hi".Length(2) + "!".Length(1) = 3
}

// ── CONTROL: multi-element inline array with record element.
// per §15.1 / §8.6 (issue #4331, control)
// See note above: switched to `[2 owned]` for #4351.
BEZEL_TEST(lambda_inline_array_interface_element_4331,
           record_element_multi_elem_expr_bodied_lambda_spec_15_1_8_6) {
    BEZEL_EXPECT_EQ(run(
        kPrelude +
        "class C "
        "  public static method Main(): int "
        "    var f: () -> [2 owned]IHolder := lambda() => [new RecHolder(\"hi\"), new RecHolder(\"!\")]; "
        "    var outer := f(); "
        "    return outer[0].GetV().Length + outer[1].GetV().Length; "
        "  end Main; "
        "end C;"),
        3);  // "hi".Length(2) + "!".Length(1) = 3
}