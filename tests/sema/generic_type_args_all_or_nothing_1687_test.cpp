// Spec-derived sema tests for fix #1687 — §14.5 (v0.184) "Explicit lists
// are all-or-nothing" and the §14.5 Scope paragraph "class-, record-, and
// interface-level type arguments are never inferred from constructor
// arguments."
//
// §14.5 verbatim (docs/Bezel_Language_Spec.md, "Explicit lists are
// all-or-nothing"):
//   "An explicit type-argument list (`obj.Method<int>(args)` ...) binds
//    the declared type parameters in declaration order and must supply
//    exactly as many arguments as the method declares type parameters. A
//    shorter or a longer list is a compile error; there are no partial
//    lists. When a full explicit list is supplied, no inference runs."
//
// §14.5 verbatim ("Scope" paragraph):
//   "Class-, record-, and interface-level type arguments are never
//    inferred from constructor arguments: `new Pair(1, "x")` against
//    `record Pair<TF, TS>(First: TF, Second: TS)` is a compile error
//    directing to explicit arguments (`new Pair<int, string>(1, "x")`) or,
//    where a target type exists, the `new()` shorthand (§17.7)."
//
// Before the fix, bezelc mishandled both halves of this rule (issue #1687):
//   - a partial explicit method type-argument list (fewer args than
//     declared type parameters) was silently accepted, leaving the
//     remaining type parameters unbound with no diagnostic and no
//     inference run to fill them;
//   - an excess explicit list (more args than declared type parameters)
//     was silently accepted, the extra arguments simply ignored;
//   - `new Pair(1, "x")` against a generic record/class with NO explicit
//     type arguments and no target-type context was silently accepted,
//     leaving the constructor's field types as unresolved type
//     parameters — this either miscompiled (a member read through the
//     unresolved type) or crashed the backend outright (an
//     ICmpInst::AssertOK assertion failure comparing the unresolved-typed
//     field against a differently-typed value, confirmed via a standalone
//     bezelc repro during the fix's verification pass).
//
// These tests are load-bearing: each REJECTED case FAILS (no diagnostic)
// on pre-fix bezelc and PASSES (the §14.5 diagnostic fires) post-fix; each
// ACCEPTED case must remain diagnostic-free both before and after (positive
// controls guarding against over-rejection of the valid all-or-nothing and
// full-inference forms).

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

// The §14.5 "all-or-nothing" method-call diagnostic (#1687).
bool hasAllOrNothingError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("no partial lists") != std::string::npos &&
            d.message.find("14.5") != std::string::npos)
            return true;
    return false;
}

// The §14.5 Scope "no constructor inference" diagnostic (#1687).
bool hasNoConstructorInferenceError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("never inferred from constructor arguments") !=
                std::string::npos &&
            d.message.find("14.5") != std::string::npos)
            return true;
    return false;
}

bool hasAnyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) return true;
    return false;
}

// Two-type-parameter generic static method shared by the method-call tests.
const char* kPickClass =
    "static class Utils "
    "  public static method Pick<TFirst, TSecond>(a: TFirst, b: TSecond): "
    "TFirst "
    "    return a; "
    "  end Pick; "
    "end Utils; ";

}  // namespace

// §14.5 / #1687 (p3): a SHORTER explicit list (1 of 2 declared type
// parameters) is a compile error, not a partially-inferred call.
BEZEL_TEST(generic_type_args_1687, partial_explicit_list_rejected) {
    auto r = parseSemaAll(
        "module M; " + std::string(kPickClass) +
        "class C "
        "  public static method Main(): int "
        "    var x := Utils.Pick<int>(1, \"s\"); "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasAllOrNothingError(r));
}

// §14.5 / #1687 (p11): a LONGER explicit list (3 of 2 declared type
// parameters) is a compile error — extras are not silently dropped.
BEZEL_TEST(generic_type_args_1687, excess_explicit_list_rejected) {
    auto r = parseSemaAll(
        "module M; " + std::string(kPickClass) +
        "class C "
        "  public static method Main(): int "
        "    var x := Utils.Pick<int, string, bool>(1, \"s\"); "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasAllOrNothingError(r));
}

// §14.5 / #1687: the same partial-list rule applies to a bare-identifier
// (no-dot, same-class) generic call, not just the dotted member-access form.
BEZEL_TEST(generic_type_args_1687, partial_explicit_list_bare_identifier_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Pick<TFirst, TSecond>(a: TFirst, b: "
        "TSecond): TFirst "
        "    return a; "
        "  end Pick; "
        "  public static method Main(): int "
        "    var x := Pick<int>(1, \"s\"); "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasAllOrNothingError(r));
}

// Positive control: a FULL explicit type-argument list (2 of 2) is
// accepted — the all-or-nothing check must not over-reject the valid form.
BEZEL_TEST(generic_type_args_1687, full_explicit_list_accepted) {
    auto r = parseSemaAll(
        "module M; " + std::string(kPickClass) +
        "class C "
        "  public static method Main(): int "
        "    var x := Utils.Pick<int, string>(1, \"s\"); "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAllOrNothingError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// Positive control: NO explicit type-argument list — full argument-driven
// inference (§14.5 phase 1) must still work and stay diagnostic-free.
BEZEL_TEST(generic_type_args_1687, no_list_full_inference_accepted) {
    auto r = parseSemaAll(
        "module M; " + std::string(kPickClass) +
        "class C "
        "  public static method Main(): int "
        "    var x := Utils.Pick(1, \"s\"); "
        "    return x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasAllOrNothingError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

// §14.5 Scope / #1687 (q19): `new Pair(1, "x")` against a generic RECORD
// with no explicit type arguments and no target-type context is a compile
// error — class-/record-level type arguments are never inferred from
// constructor arguments. This is the spec's own example.
BEZEL_TEST(generic_type_args_1687, record_ctor_no_type_args_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record Pair<TFirst, TSecond>(First: TFirst, Second: TSecond); "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pair(1, \"x\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNoConstructorInferenceError(r));
}

// §14.5 Scope / #1687: the same rule applies to a generic CLASS constructor.
BEZEL_TEST(generic_type_args_1687, class_ctor_no_type_args_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  private _v: T; "
        "  public constructor(v: T) "
        "    _v := v; "
        "  end constructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box(5); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNoConstructorInferenceError(r));
}

// Positive control: `new Pair<int, string>(1, "x")` — the spec's own
// suggested fix for q19 — is accepted (a full explicit list on the
// constructor call).
BEZEL_TEST(generic_type_args_1687, record_ctor_full_explicit_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "record Pair<TFirst, TSecond>(First: TFirst, Second: TSecond); "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pair<int, string>(1, \"x\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNoConstructorInferenceError(r));
}

// Positive control: the §17.7 new() shorthand with a typed local target
// resolves the class-level type arguments from the DECLARED TYPE, not from
// the constructor arguments — this is not "inference from constructor
// arguments", so §14.5's Scope paragraph does not forbid it.
BEZEL_TEST(generic_type_args_1687, record_ctor_target_typed_shorthand_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "record Pair<TFirst, TSecond>(First: TFirst, Second: TSecond); "
        "class C "
        "  public static method Main(): int "
        "    var p: Pair<int, string> := new(1, \"x\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNoConstructorInferenceError(r));
}

// Regression control: a NON-generic class constructor (no type parameters
// at all) must remain completely unaffected by the §14.5 Scope check.
BEZEL_TEST(generic_type_args_1687, nongeneric_ctor_unaffected) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget "
        "  private _v: int; "
        "  public constructor(v: int) "
        "    _v := v; "
        "  end constructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(7); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNoConstructorInferenceError(r));
}
