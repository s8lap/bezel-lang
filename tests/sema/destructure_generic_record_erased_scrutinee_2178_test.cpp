// Spec-derived tests for §19.3 (v0.226, #2178): the generic-record
// DESTRUCTURING pattern's erasure guard.
//
// §19.3 (the per-instantiation §19.3 record destructuring spelling):
// "a record-destructuring pattern's positional-binding count matches the
// record's positional-field count." Today the destructuring-pattern path
// resolves the destructured record's SHARED per-declaration descriptor and
// binds the fields at TEMPLATE offsets — a box of ANY instantiation is
// accepted and the arm's bindings land at the WRONG static type, the same
// memory-unsafe class the #2001/#2006/#2012/#2018 erasure guards close
// (descTypeArgConcrete / typeArgRuntimeReified / appendDescKey collapses
// `Kind::Function` and an unfolded inline array across instantiations).
//
// A `Pair(a, b)` pattern (no type args) against a generic record
// `record Pair<A, B>(...)` carries NO static information about the
// instantiation; today the pattern matches a `Pair<int, string>` AND a
// `Pair<string, int>` and binds the fields at template offsets — a
// wrong-typed read when two instantiations coexist. The rejection site is
// sema's DestructuringPattern handler: when the destructured record is
// generic AND the scrutinee's static type does not determine the
// instantiation, REJECT. The scrutinee's `Object` is the canonical erased
// case; a non-generic interface and a class scrutinee are likewise erased
// (a class has no relationship to the destructured record, and a
// non-generic interface cannot carry the record's instantiation in its
// args). A scrutinee of the destructured record itself (the
// `p: Pair<int, string>` case) is the admit path; the typed binding
// `var p: Pair<int, string> => ...` already in the surface remains the
// precise spelling.
//
// Both CaseStmt and CaseExpr handlers in src/sema/ExprTypecheck(.cpp/.Exprs
// .cpp) get the same guard. The probe matrix below exercises every axis:
//   - scrutinee kind: Object (REJECT), non-generic interface (REJECT),
//     class (REJECT), a generic interface whose arg is the record (REJECT —
//     scrutinee does not pin the destructured record's instantiation either
//     way), and a concrete record scrutinee (ACCEPT).
//   - statement vs expression form of the case (mirror both handlers).
//   - non-generic record (no `genericParams`): the guard does not fire —
//     the existing per-declaration destructure is the sound shape (ACCEPT).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff an ERROR diagnostic carries the #2178 generic-destructure reason.
// The phrase "TEMPLATE layout" is unique to this guard.
bool hasGenericDestructureErasedError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("TEMPLATE layout") != std::string::npos)
            return true;
    }
    return false;
}

// True iff an ERROR diagnostic carries the #2178 generic-destructure reason
// with the specific #2178 issue tag — the diagnostic names it.
bool hasGenericDestructureIssueTag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("#2178") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// =============================================================================
// REJECTED — the headline cases. A generic-record destructure pattern against
// an ERASED scrutinee (Object, class, non-generic interface, generic interface
// whose arg is the record's enclosing generic interface — none of these pin
// the destructured record's instantiation). FAILS WITHOUT (the arm compiled);
// PASSES WITH the fix.
// =============================================================================

// (1) STATEMENT form: Object scrutinee, generic `Pair<A,B>` destructured, the
//     original #2178 repro / pre-fix #1939-regression shape. REJECTED.
BEZEL_TEST(destructure_generic_record_erased_scrutinee_2178,
           stmt_object_scrutinee_generic_pair_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "record Pair<A, B>(first: A, second: B);\n"
        "class Program\n"
        "  public method M(o: Object): int\n"
        "    case o of\n"
        "      Pair(a, b) => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(hasGenericDestructureErasedError(r));
    BEZEL_EXPECT_TRUE(hasGenericDestructureIssueTag(r));
}

// (2) EXPRESSION form: same shape via the value-position CaseExpr handler.
//     REJECTED — proves the guard runs on both the statement and expression
//     destructuring paths.
BEZEL_TEST(destructure_generic_record_erased_scrutinee_2178,
           expr_object_scrutinee_generic_pair_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "record Pair<A, B>(first: A, second: B);\n"
        "class Program\n"
        "  public method M(o: Object): int\n"
        "    return (case o of\n"
        "      Pair(a, b) => 1,\n"
        "      _ => 0);\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(hasGenericDestructureErasedError(r));
    BEZEL_EXPECT_TRUE(hasGenericDestructureIssueTag(r));
}

// (3) A CLASS scrutinee (erased — no relationship to the destructured record).
//     REJECTED — proves the erased-kind detection is broader than Object.
BEZEL_TEST(destructure_generic_record_erased_scrutinee_2178,
           stmt_class_scrutinee_generic_pair_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "record Pair<A, B>(first: A, second: B);\n"
        "class Box\nend Box;\n"
        "class Program\n"
        "  public method M(b: Box): int\n"
        "    case b of\n"
        "      Pair(a, c) => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(hasGenericDestructureErasedError(r));
}

// (4) A NON-generic interface scrutinee (erased — no args to carry the
//     destructured record's instantiation). REJECTED.
BEZEL_TEST(destructure_generic_record_erased_scrutinee_2178,
           stmt_nongeneric_iface_scrutinee_generic_pair_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "record Pair<A, B>(first: A, second: B);\n"
        "interface IThing\nend IThing;\n"
        "class Program\n"
        "  public method M(o: IThing): int\n"
        "    case o of\n"
        "      Pair(a, b) => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(hasGenericDestructureErasedError(r));
}

// (5) A GENERIC-INTERFACE scrutinee whose arg is the destructured record
//     itself. The scrutinee is `IBox<Pair<int, string>>` and the pattern
//     destructures `Pair(a, b)` — the scrutinee's generic-arg is the
//     destructured record, but the pattern's destructure target is the
//     record's TEMPLATE (no type args), so it would still bind at template
//     offsets. The strict reading: the scrutinee does not pin the
//     destructured record's instantiation through a Named whose declaration
//     IS the record (scoutTy->declaration != recTy). REJECTED — pins the
//     "scrutTy's declaration != recTy" arm of the guard.
BEZEL_TEST(destructure_generic_record_erased_scrutinee_2178,
           stmt_iface_scrutinee_holding_record_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "record Pair<A, B>(first: A, second: B);\n"
        "interface IBox<T>\nend IBox;\n"
        "class Program\n"
        "  public method M(o: IBox<Pair<int, string>>): int\n"
        "    case o of\n"
        "      Pair(a, b) => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(hasGenericDestructureErasedError(r));
}

// =============================================================================
// ACCEPTED — guards must not over-reject. Each test produces a sound-accept
// program (FAILS WITH the fix, PASSES without). These are the controls that
// pin the guard's strictness: the destructured record's INSTANTIATION is
// determinable from the scrutinee, OR the destructured record is not generic
// (the per-declaration destructure remains sound).
// =============================================================================

// (6) STATEMENT form: a CONCRETE record scrutinee of the same record type.
//     `p: Pair<int, string>` with a `Pair(a, b)` pattern. ACCEPTED — the
//     scrutinee statically pins the destructured record's instantiation; the
//     guard does not fire (scoutTy->declaration == recTy, AND recTy IS the
//     scrutinee type).
BEZEL_TEST(destructure_generic_record_erased_scrutinee_2178,
           stmt_concrete_record_scrutinee_same_record_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "record Pair<A, B>(first: A, second: B);\n"
        "class Program\n"
        "  public method M(p: Pair<int, string>): int\n"
        "    case p of\n"
        "      Pair(a, b) => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(hasGenericDestructureErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (7) EXPRESSION form: same shape via the value-position CaseExpr handler.
//     ACCEPTED — proves the guard on the CaseExpr side does not over-reject
//     the concrete record scrutinee case.
BEZEL_TEST(destructure_generic_record_erased_scrutinee_2178,
           expr_concrete_record_scrutinee_same_record_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "record Pair<A, B>(first: A, second: B);\n"
        "class Program\n"
        "  public method M(p: Pair<int, string>): int\n"
        "    return (case p of\n"
        "      Pair(a, b) => 1,\n"
        "      _ => 0);\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(hasGenericDestructureErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (8) NON-generic record (the guard's `recTy->genericParams` check is the
//     sole trigger). `Point` has no `<A,B>`; the destructure is the
//     per-declaration shape and is sound. ACCEPTED — pins the guard's
//     non-over-rejection on the common case.
BEZEL_TEST(destructure_generic_record_erased_scrutinee_2178,
           stmt_nongeneric_record_against_object_scrutinee_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "record Point(x: int, y: int);\n"
        "class Program\n"
        "  public method M(o: Object): int\n"
        "    case o of\n"
        "      Point(x, y) => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(hasGenericDestructureErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (9) The precise §19.3 typed binding (the issue's recommended spelling) is
//     unaffected by the new guard. `var p: Pair<int, string> => ...` is a
//     BINDING pattern, not a DESTRUCTURING pattern — the guard's
//     `DestructuringPattern` arm does not engage. ACCEPTED.
BEZEL_TEST(destructure_generic_record_erased_scrutinee_2178,
           stmt_typed_binding_pattern_against_object_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "record Pair<A, B>(first: A, second: B);\n"
        "class Program\n"
        "  public method M(o: Object): int\n"
        "    case o of\n"
        "      var p: Pair<int, string> => do return 1; end,\n"
        "      _ => do return 0; end\n"
        "    end;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(hasGenericDestructureErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
