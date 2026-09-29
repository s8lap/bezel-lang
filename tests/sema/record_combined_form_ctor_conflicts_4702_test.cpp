// fix #4702 (§9.3 "Combined form" / §9.9 "Constructor-only records", v0.307):
// a combined-form record (positional fields + a body, §9.3) whose body
// declares a constructor carrying a `requires`/`ensures` contract clause
// (§22.16) is a compile error — "The body may not declare a constructor
// carrying a contract clause ... so no record declaring positional fields is
// constructor-only" (§9.9's own first paragraph states the same rule
// directly: "A record declaring positional fields may not declare such a
// constructor: a compile error at the record declaration").
//
// This check did not exist anywhere in `src/sema/` before this fix —
// confirmed by grepping the whole tree for the rule's wording and for any
// per-member walk reading a body constructor's `contractClauses`. The
// program below compiled clean (exit 0) pre-fix instead of being rejected.
//
// §9.3 states a SECOND body-constructor disqualifier this fix does NOT
// implement — "same parameter list as the auto-generated primary
// constructor" — because implementing it (read as parameter-TYPE shape,
// matching every other signature-shape rule in this codebase) rejects a
// pre-existing, apparently deliberate lit test
// (sema/sema_serialization_550__record_combined_form_explicit_ctor_match_
// accepted_spec_9_8_v0182.bzl) that has been ACCEPTING exactly that shape
// (constructor(x: int, y: int) against positional fields X, Y — same types,
// case-different names) since before this fix, to exercise §9.8's own
// "Constructor selection ... matched by name ... compared case-
// insensitively" rule. See issue #4702 for the full analysis; test (3) below
// pins that pre-existing acceptance as a regression guard.
//
// Corpus row this fix moves (`tests/conformance/rules/cases/`, banked
// centrally, not by this lane): s9_3_0004. (s9_3_0002 and s9_9_0008, the
// "same parameter list" rows, are NOT moved — left `fail`/`KNOWN` pending
// #4702's resolution.)
//
// THE FIX: `Resolver::checkRecordCombinedFormContractClauseConstructors()`
// (src/sema/TypeResolution.cpp, declared in TypeResolution_impl.hpp, called
// from `Resolver::run()` right after `checkRecordRestrictions()`).
//
// FAIL-WITHOUT / PASS-WITH (proven mechanically): with the function's body
// reverted to a no-op (and its call site removed), test (1) below instead
// compiles clean (`r.ran` true, the constructor's own logic actually
// running); restored, it asserts a real §9.9-cited compile-time location and
// no artifact is produced. The other tests compile and run identically on
// both sides — this fix touches no accepted program.
//
// Filter run: `--area sema record_combined_form_ctor_conflicts`.
//
// Writing tests (2) and (4) below surfaced a SEPARATE, unrelated pre-
// existing defect (confirmed present with this fix fully reverted): `new
// T(...)` overload resolution for a combined-form record never considers a
// differently-shaped body constructor as a candidate — filed as issue
// #4712. Both tests construct via the PRIMARY constructor's own shape to
// stay clear of it; see each test's own comment.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── (1) pin THE BUG: the issue's exact §9.3/§9.9 repro — a DIFFERENTLY-
//        shaped body constructor (2 params vs. the primary's 1) that carries
//        a `requires` clause — proves the check fires on the clause alone,
//        independent of any parameter-shape comparison ───────────────────
BEZEL_TEST(record_combined_form_ctor_conflicts_4702,
           contract_clause_rejected_regardless_of_shape_spec_9_3_9_9) {
    auto r = runAot(
        "module M; "
        "public record Port(Value: int) "
        "  public constructor(v: int, forced: bool) "
        "    requires(v >= 0 and v <= 65535) "
        "    this(v); "
        "  end constructor; "
        "end Port; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Port(1); "
        "    return p.Value; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "carrying a contract clause"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.9"));
}

// ── (2, companion / positive control) a body constructor with a DIFFERENT
//        parameter-type shape and NO contract clause is admitted — §9.3:
//        "A body constructor with a different parameter list is not
//        required to chain to the primary" — proves the check is not a
//        blanket rejection of every body constructor.
//
//        NOTE: this constructs via the PRIMARY shape (`new Point2(1, 2)`),
//        not the body constructor's own 1-arg shape. Calling `new
//        Point2(5)` to actually invoke the differently-shaped body
//        constructor hits an UNRELATED, separately-filed pre-existing
//        defect (issue #4712: `new T(...)` overload resolution for a
//        combined-form record does not consider a differently-shaped body
//        constructor as a candidate at all — confirmed present on
//        unmodified `r19-c20` HEAD, independent of this fix). Constructing
//        via the primary's own shape is sufficient to prove THIS fix does
//        not reject the DECLARATION. ─────────────────────────────────────
BEZEL_TEST(record_combined_form_ctor_conflicts_4702,
           different_shape_no_clause_accepted_positive_control) {
    auto r = runAot(
        "module M; "
        "public record Point2(X: int, Y: int) "
        "  public constructor(scale: int) "
        "    X := scale; "
        "    Y := scale; "
        "  end constructor; "
        "end Point2; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Point2(1, 2); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ── (3, companion / positive control / #4702 regression guard) a body
//        constructor whose parameter TYPES match the primary's but whose
//        NAMES differ only in case, and carries NO contract clause, stays
//        ACCEPTED — this is the exact shape sema_serialization_550's lit
//        test already relies on for §9.8's case-insensitive constructor
//        selection; pinned here so a future attempt at §9.3's "same
//        parameter list" disqualifier trips over this test rather than
//        silently reintroducing the #4702 conflict ─────────────────────────
BEZEL_TEST(record_combined_form_ctor_conflicts_4702,
           same_type_shape_different_case_names_accepted_positive_control) {
    auto r = runAot(
        "module M; "
        "public record Vec2b(X: int, Y: int) "
        "  public constructor(x: int, y: int) "
        "    X := x + 100; "
        "    Y := y + 100; "
        "  end constructor; "
        "end Vec2b; "
        "public class Program "
        "  public static method Main(): int "
        "    var v := new Vec2b(1, 2); "
        "    return v.X + v.Y; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 203);  // (1+100) + (2+100)
}

// ── (4, companion / positive control) a different-shaped body constructor
//        chaining to the primary via `this(...)` — §9.3: "Chaining with
//        `this(...)` to the primary remains available" — must still be
//        admitted (declaration compiles clean).
//
//        NOTE: as in (2) above, `Main` constructs via the primary's own
//        shape rather than the body constructor's — invoking the latter
//        via `new Point3(4)` hits the same unrelated #4712 defect. This
//        still exercises everything THIS fix could plausibly break: the
//        `this(v, v)` chain's own declaration-time checks (chain-target
//        existence, argument-count/type matching against the primary) run
//        regardless of whether the constructor is ever called. ───────────
BEZEL_TEST(record_combined_form_ctor_conflicts_4702,
           different_shape_this_chain_accepted_positive_control) {
    auto r = runAot(
        "module M; "
        "public record Point3(X: int, Y: int) "
        "  public constructor(v: int) "
        "    this(v, v); "
        "  end constructor; "
        "end Point3; "
        "public class Program "
        "  public static method Main(): int "
        "    var p := new Point3(3, 5); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}

// ── (5, companion / positive control) an ordinary CLASS (never a record) is
//        untouched by this record-only rule — the check's own first line is
//        `t->kind != ast::TypeDeclKind::Record`, so a class constructor
//        carrying a contract clause — legal per §22.16, no §9.9 property
//        applies to a class at all — must keep compiling and running ──────
BEZEL_TEST(record_combined_form_ctor_conflicts_4702,
           class_contract_clause_untouched_positive_control) {
    auto r = runAot(
        "module M; "
        "public class Box "
        "  public Value: int; "
        "  public constructor(v: int) "
        "    requires(v >= 0) "
        "    Value := v; "
        "  end constructor; "
        "end Box; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box(9); "
        "    return b.Value; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// ── (6, companion / positive control) a body-ONLY record (no positional
//        fields at all) may declare a contract-clause constructor freely —
//        §9.9's trigger is "declares POSITIONAL fields"; a body-only record
//        is never that, so the property is simply what it always was
//        (constructor-only is fine when there is no auto-generated primary
//        constructor to conflict with) ─────────────────────────────────────
BEZEL_TEST(record_combined_form_ctor_conflicts_4702,
           body_only_record_contract_clause_accepted_positive_control) {
    auto r = runAot(
        "module M; "
        "public record Ratio "
        "  public readonly property Value: int; "
        "  public constructor(v: int) "
        "    requires(v >= 0) "
        "    Value := v; "
        "  end constructor; "
        "end Ratio; "
        "public class Program "
        "  public static method Main(): int "
        "    var r := new Ratio(3); "
        "    return r.Value; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}
