// Regression tests for issue #5132 (§9.3 / §11.1 / §22.6, v0.308): the
// "same parameter list as the auto-generated primary constructor" clash
// check uses §22.6 overload identity (arity, parameter types, `ref`/`out`
// modifiers, ordinary `owned`); parameter NAMES and DEFAULTS do not
// distinguish.
//
// Pre-fix v0.307 narrow predicate (the §11.1 class-side check
// `Resolver::checkTypeConstructorChains` in
// src/sema/TypeResolution_Checks.cpp:514-584; the §9.3 record-side check
// in src/sema/TypeResolution.cpp ~769-820) also compared parameter NAMES
// case-sensitively AND the presence of a default value. The v0.308 ruling
// (#4775 Q1/Q3/Q4 close) says both names and defaults are NOT part of
// overload identity and so do NOT exempt a body constructor from §11.1 /
// §9.3.
//
// The five rows below cover:
//   T1 — record combined-form: `record Point(X, Y)` + `constructor(x, y)`
//        (the issue's headline repro). REJECT.
//   T2 — class body ctor differing from the primary only by default
//        presence. REJECT.
//   T3 — class body ctor differing from the primary only by parameter
//        names (the #3751 owned-pair shape). REJECT.
//   T4 — non-regression guards: a body ctor with truly different types or
//        arity is still ACCEPTED (and a name-changed body ctor that still
//        does not match under §22.6 is ACCEPTED).
//   T5 — the existing tests whose rows pinned the v0.307 (name/default)
//        rule now reject: the issue's three named sites (4712 rows (3)
//        and (4), the #4001 combined-form accept row, the #3751 lit row),
//        restated here as REJECT rows so this file alone fully documents
//        the flip and a single fix-forwards-back lands them all.
//
// FAIL-WITHOUT (MEASURED, sema change reverted, rest of branch intact):
// T1, T2, T3, and all five T5 rows compile clean and pass an accept oracle;
// with the fix in place, they emit the §11.1 / §9.3 redeclares-primary
// diagnostic and never reach codegen.
//
// Spec: §9.3 (records, the auto-generated primary is "the same parameter
// list as" the body ctor in §11.3's clash sense), §11.1 (classes, same
// sentence), §11.3 ("two constructors with the same name that are not
// distinguishable as overloads (§22.6)"), §22.6 (overload identity —
// arity, parameter types, `ref`/`out` modifiers, ordinary `owned` on a
// participating position; parameter names and defaults do not distinguish),
// v0.308.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains,
                                            // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Convenience: assert the program is rejected at compile time with the
// §11.1 / §9.3 redeclares-primary diagnostic, naming the spec section the
// source declares (§11.1 for classes, §9.3 for records).
void expectClashRejected(const std::string& program,
                         const char* spec) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "body constructor redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, spec));
}

}  // namespace

// ── T1 — RECORD combined-form: `record Point(X: int, Y: int)` +
//        `constructor(x: int, y: int)` — same types / same arity, names
//        differ. Under §22.6 the parameter lists are identical; §9.3
//        REJECTS. (The issue's headline repro, §9.3 / §22.6, v0.308.) ────
BEZEL_TEST(same_param_list_clash_uses_overload_identity_5132,
           record_combined_form_differs_only_by_names_rejected_spec_9_3) {
    expectClashRejected(
        "module M; "
        "public record Point(X: int, Y: int) "
        "  public constructor(x: int, y: int) "
        "  end constructor; "
        "end Point; "
        "public class P "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end P;",
        "\xC2\xA7""9.3");
}

// ── T2 — CLASS body ctor differing from the primary ONLY by default
//        presence. The primary is `(a: int)`; the body ctor is
//        `(b: int := 0)`. §22.6 strips the default (defaults do not
//        distinguish); the lists match → §11.1 REJECTS. (The default-
//        presence pin; defaults are part of the v0.307 narrow predicate
//        and are removed by §22.6 identity.) ────────────────────────────────
BEZEL_TEST(same_param_list_clash_uses_overload_identity_5132,
           class_body_ctor_differs_only_by_default_rejected_spec_11_1) {
    expectClashRejected(
        "module M; "
        "class K(a: int) "
        "  public constructor(b: int := 0) "
        "  end constructor; "
        "end K; "
        "class P "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end P;",
        "\xC2\xA7""11.1");
}

// ── T3 — CLASS body ctor differing from the primary ONLY by parameter
//        names — the #3751 owned-pair shape. The primary is
//        `(owned r: Res)`; the body ctor is `(owned x: Res)` — same
//        type, same modifier, names differ. §22.6 identity → §11.1
//        REJECTS. (This is the issue's class-side repro.) ───────────────────
BEZEL_TEST(same_param_list_clash_uses_overload_identity_5132,
           class_body_ctor_differs_only_by_names_owned_rejected_spec_11_1) {
    expectClashRejected(
        "module M; "
        "class Res "
        "  public constructor() end constructor; "
        "end Res; "
        "class Base(owned r: Res) "
        "  public constructor(owned x: Res) "
        "  end constructor; "
        "end Base; "
        "class P "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end P;",
        "\xC2\xA7""11.1");
}

// ── T4a — non-regression: a body ctor with DIFFERENT TYPES is a true
//        overload and must be ACCEPTED. (Verifies the §22.6 type rule still
//        distinguishes.) ────────────────────────────────────────────────────
BEZEL_TEST(same_param_list_clash_uses_overload_identity_5132,
           class_body_ctor_different_types_accepted_non_regression_spec_22_6) {
    auto r = runAot(
        "module M; "
        "class K(a: int) "
        "  public constructor(s: string) "
        "  end constructor; "
        "end K; "
        "class P "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "redeclares the primary constructor"));
    // The body ctor may be diagnosed for unrelated reasons (e.g. a missing
    // `this(...)` chain), but it must NOT trigger the §11.1 redeclaration.
}

// ── T4b — non-regression: a body ctor with DIFFERENT ARITY is a true
//        overload (the §22.6 arity rule) and must be ACCEPTED. ──────────────
BEZEL_TEST(same_param_list_clash_uses_overload_identity_5132,
           class_body_ctor_different_arity_accepted_non_regression_spec_22_6) {
    auto r = runAot(
        "module M; "
        "class K(a: int, b: int) "
        "  public constructor(n: int) "
        "    this(n, 0); "
        "  end constructor; "
        "end K; "
        "class P "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "redeclares the primary constructor"));
}

// ── T5a — FLIPPED row: 4712 (3) `record R(Val: int)` + `constructor(v:
//        int)` was an accept oracle; under §9.3 / §22.6 it now REJECTS. ─────
BEZEL_TEST(same_param_list_clash_uses_overload_identity_5132,
           flipped_4712_row_3_record_R_ctor_v_rejected_spec_9_3) {
    expectClashRejected(
        "module M; "
        "public record R(Val: int) "
        "  public constructor(v: int) "
        "    Val := v; "
        "  end constructor; "
        "end R; "
        "public class Program "
        "  public static method Main(): int "
        "    var r := new R(7); "
        "    return r.Val; "
        "  end Main; "
        "end Program;",
        "\xC2\xA7""9.3");
}

// ── T5b — FLIPPED row: 4712 (4) `record Box(Count: int)` + body ctors
//        differing only by name was an accept oracle; now REJECTS. ──────────
BEZEL_TEST(same_param_list_clash_uses_overload_identity_5132,
           flipped_4712_row_4_record_Box_ctor_n_rejected_spec_9_3) {
    expectClashRejected(
        "module M; "
        "public record Box(Count: int) "
        "  public constructor(n: int) "
        "    Count := n * 10; "
        "  end constructor; "
        "end Box; "
        "public class Program "
        "  public static method Main(): int "
        "    var b := new Box(3); "
        "    return b.Count; "
        "  end Main; "
        "end Program;",
        "\xC2\xA7""9.3");
}

// ── T5c — FLIPPED row: the #4001 combined-form accept row
//        `record Combo4001OK(X: int)` + `constructor(x: int)`. Same arity,
//        same type, names differ. §9.3 REJECTS. ─────────────────────────────
BEZEL_TEST(same_param_list_clash_uses_overload_identity_5132,
           flipped_4001_combined_form_primary_ctor_body_rejected_spec_9_3) {
    expectClashRejected(
        "module M; "
        "public class Widget4001 "
        "  public method Render(): int return 0; end Render; "
        "end Widget4001; "
        "public record Combo4001OK(X: int) "
        "  public readonly property Extra: Widget4001; "
        "  public constructor(x: int) "
        "    X := x; "
        "    Extra := new Widget4001(); "
        "  end constructor; "
        "end Combo4001OK; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Combo4001OK(5); "
        "    return c.X; "
        "  end Main; "
        "end C;",
        "\xC2\xA7""9.3");
}

// ── T5d — FLIPPED row: the #3751 lit row
//        `class Base(owned r: Res)` + `constructor(owned x: Res)` was an
//        accept oracle; under §11.1 / §22.6 it now REJECTS. ─────────────────
BEZEL_TEST(same_param_list_clash_uses_overload_identity_5132,
           flipped_3751_owned_convenience_ctor_rejected_spec_11_1) {
    expectClashRejected(
        "module M; "
        "class Res "
        "  private _id: int; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  public method Id(): int => _id; "
        "end Res; "
        "class Base(owned r: Res) "
        "  private owned _s: Res := r; "
        "  public constructor(owned x: Res) "
        "    this(owned x); "
        "  end constructor; "
        "  public method Id(): int return _s.Id(); end Id; "
        "end Base;",
        "\xC2\xA7""11.1");
}

// ── T5e — FLIPPED row: the §11.3 / §22.6 record Vec2 combined-form
//        (from 4839 row 6 / 5100 row 6 / the #2362-style pattern). The
//        record's primary is `Vec2(X: int, Y: int)`; the body ctor is
//        `constructor(x: int, y: int)`. Names differ only; under
//        §9.3 / §22.6 it now REJECTS. (This row covers the ACCEPT arms of
//        4839 row 6, 5100 row 6, and the §9.8 step-2 Vec2 lit row that all
//        pinned the v0.307 predicate.) ─────────────────────────────────────
BEZEL_TEST(same_param_list_clash_uses_overload_identity_5132,
           flipped_4839_5100_vec2_combined_form_rejected_spec_9_3) {
    expectClashRejected(
        "module M; "
        "record Vec2(X: int, Y: int) "
        "  public constructor(x: int, y: int) "
        "    X := x; "
        "    Y := y; "
        "  end constructor; "
        "end Vec2; "
        "class P "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end P;",
        "\xC2\xA7""9.3");
}
