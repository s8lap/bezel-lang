// Spec-derived sema tests for fix #1690 ([v0.184] LValue over-acceptance:
// method-call bases, A.6/§6.6) — a method-call result was silently accepted
// as the base of an assignment/increment lvalue chain.
//
// Authority: grammar A.6 — `LValue ::= Identifier | "this" | LValue "."
// Identifier | LValue "[" ArgList "]"` — restricts an lvalue's BASE to an
// identifier or `this`; the grammar comment states it directly: "Method-call
// results cannot serve as the base of an assignment; `getCache().items[0] :=
// v` does not parse." §6.6 (v0.184) restates it for the compound-assignment
// desugaring: "A.6's lvalue shape applies unchanged: a method-call result
// cannot base the chain — extract it to a local first." Before the fix,
// `h.Self().total += 5;` (the shape of #1690's repro,
// types-probes/q6b_recv_once.bz's `h.Self().Total += 5;`) compiled cleanly —
// sema resolved the `total` member symbol through the chain without ever
// checking that the chain's base had a valid lvalue shape. Legitimate
// lvalues (a plain field, a `this`-qualified field, an indexer slot, and the
// null-forgiving `!` read-through of #206) must stay accepted — `!` and `?.`
// denote the SAME underlying storage as their operand, not a fresh value.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasCallBaseLValueError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(
                "method-call result cannot be the base of an assignment "
                "target") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §6.6 / A.6: a compound-assignment target whose base is a method-call
// result is rejected — the exact #1690 repro shape.
BEZEL_TEST(lvalue_call_base_1690, compound_assign_method_call_base_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public total: int := 0; "
        "  public method Self(): C => this; "
        "  public static method Main(): int "
        "    using var h := new C(); h.Self().total += 5; return h.total; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCallBaseLValueError(r));
}

// §6.6 / A.6: plain `:=` through a method-call base is rejected too — not
// only the compound-assignment forms.
BEZEL_TEST(lvalue_call_base_1690, plain_assign_method_call_base_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public total: int := 0; "
        "  public method Self(): C => this; "
        "  public static method Main(): int "
        "    using var h := new C(); h.Self().total := 5; return h.total; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCallBaseLValueError(r));
}

// §6.6 / A.6 (`IncrementStmt ::= LValue ("++"|"--")`): the same restriction
// applies to `++`/`--`, which consume the same grammar A.6 `LValue`
// production as AssignmentStmt.
BEZEL_TEST(lvalue_call_base_1690, increment_method_call_base_rejected) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public total: int := 0; "
        "  public method Self(): C => this; "
        "  public static method Main(): int "
        "    using var h := new C(); h.Self().total++; return h.total; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasCallBaseLValueError(r));
}

// Positive control: a plain field target (no call anywhere in the chain) is
// unaffected by the new check.
BEZEL_TEST(lvalue_call_base_1690, plain_field_target_accepted) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public total: int := 0; "
        "  public static method Main(): int "
        "    using var h := new C(); h.total += 5; return h.total; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCallBaseLValueError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Positive control: a `this`-qualified target inside the declaring class is
// unaffected.
BEZEL_TEST(lvalue_call_base_1690, this_qualified_target_accepted) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public total: int := 0; "
        "  public method Bump() this.total += 5; end Bump; "
        "  public static method Main(): int "
        "    using var h := new C(); h.Bump(); return h.total; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCallBaseLValueError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Positive control (#206 regression guard): the null-forgiving `!` hop reads
// through the SAME storage location as its operand, not a fresh value — it
// must remain a valid lvalue base and must NOT be flagged by this check.
BEZEL_TEST(lvalue_call_base_1690, null_forgiving_bang_target_accepted) {
    auto r = parseSemaAll(
        "module M; class C public total: int := 0; end C; "
        "class D public static method Main(): int "
        "  using var x := new C(); var a: C? := x; a!.total += 5; "
        "  return a!.total; "
        "end Main; end D;");
    BEZEL_EXPECT_FALSE(hasCallBaseLValueError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Positive control: an indexer slot on a plain-identifier base is unaffected.
BEZEL_TEST(lvalue_call_base_1690, indexer_slot_target_accepted) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public owned items: Array<int>; "
        "  public constructor() items := new Array<int>(3); end constructor; "
        "  public static method Main(): int "
        "    using var h := new C(); h.items[0] += 1; return h.items[0]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasCallBaseLValueError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
