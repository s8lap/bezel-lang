// Spec-derived regression test for issue #4935
// (spec §14.5 phase 3, §5.1, §7.5; v0.307).
//
// THE DEFECT. The argument-inference loop in `typeCall`
// (ExprTypecheck_Calls.cpp L4932-L5065) was running even when EXPLICIT
// type arguments were provided. The block's preamble comment said it
// should only run for inference — "this path only runs when typeArgs
// is empty — i.e., type inference from arguments is needed" — but the
// guard `!hadExplicitTypeArgs` was never actually applied. The
// consequence: a valid explicit-type-arg call such as
//
//   Util.Map<int?>(5, lambda(y: int?) => "n")
//
// was being rejected with the §14.1/#896 "inconsistent type-argument
// inference" diagnostic, because arg-vs-param unification against the
// bare parameter type `T` (rather than the substituted `int?`) produced
// a conflict between `int` (the value 5's type) and `int?` (the
// substituted formal for the second parameter's lambda slot).
//
// MEASURED on the pre-fix compiler (8329e032):
//   `Map<int?>(5, lambda(y: int?) => "n")` rejected with
//   "inconsistent type-argument inference: type parameter `T` cannot
//   be bound to both `int` and a different type — all arguments for a
//   type parameter must agree (§14.1, #896)".
//
// §14.5 phase 3 (v0.307) is the authority: explicit type arguments
// take priority, and inference only runs for the omitted ones. With
// explicit args supplied, the inference block is unnecessary — the
// ordinary arg-vs-param §5.1 check that runs later in
// `typeCall`/`checkCallArity` handles non-convertible cases (e.g.
// `Merge<int>(5, true)` rejects because `bool` is not implicitly
// convertible to `int`).
//
// The fix: gate the inference block on `!hadExplicitTypeArgs`. MEASURED
// the §5.1 check catches `Merge<int>(5, true)` post-fix with the
// expected diagnostic ("the argument `y` to call to `Merge` has a type
// that is not implicitly convertible to the target type (§5.1): between
// integers and `bool` is never implicit"). No additional assignability
// walk is required.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// True iff some emitted diagnostic mentions the §14.1/#896 inconsistent
// type-argument inference diagnostic text.
bool has896Diag(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("inconsistent type-argument inference") !=
                std::string::npos) {
            return true;
        }
    }
    return false;
}

// True iff some emitted diagnostic mentions the §5.1 non-convertibility
// diagnostic text (the one that fires post-fix for the hard constraint).
bool has51Diag(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not implicitly convertible to the target type") !=
                std::string::npos &&
            d.message.find("§5.1") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// True iff some emitted diagnostic mentions the §14.5 phase-3
// collection-literal-only-source inference failure (the #4895 safety
// net).
bool has4895Diag(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("phase 3 of §14.5") != std::string::npos &&
            d.message.find("collection literal") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasAnyError(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error) return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// T1 (issue #4935 repro, typed lambda variant):
// `Util.Map<int?>(5, lambda(y: int?) => "n")`.
//
// Pre-fix REJECT (the spurious "inconsistent type-argument inference").
// Post-fix ACCEPT — `5` is implicitly convertible to `int?` (§5.1), the
// lambda's `y: int?` matches the substituted formal, and the body prints
// the lambda's returned string. Exact stdout must be `"n\n"`.
//
// Verified via runAot AND runAotChecked (main's condition 2).
// ===========================================================================
BEZEL_TEST(explicit_type_arg_drives_inference_4935,
           t1_typed_lambda_with_explicit_int_nullable_runs_spec_14_5) {
    const char* prog =
        "module M; "
        "public class Util "
        "  public static method Map<T>(v: T, f: (T) -> string): string "
        "    return f(v); "
        "  end Map; "
        "end Util; "
        "public class P "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Map<int?>(5, lambda(y: int?) => \"n\")); "
        "    return 0; "
        "  end Main; "
        "end P;";
    {
        auto r = runAot(prog);
        BEZEL_EXPECT_FALSE(hasAnyError(r));
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("n\n"));
    }
    {
        auto r = runAotChecked(prog);
        BEZEL_EXPECT_FALSE(hasAnyError(r));
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("n\n"));
    }
}

// ===========================================================================
// T2 (untyped-lambda control):
// `Util.Map<int?>(5, lambda(y) => "n")` — y has no declared type.
//
// MEASURED on the pre-fix compiler: this already ACCEPTs (the untyped-y
// unification does not produce the int vs int? conflict that the typed
// variant does — the lambda's parameter type is inferred to match the
// call's substituted formal through a different path). The row pins that
// the post-fix compiler still accepts this case and prints `"n\n"`.
// Pre-fix and post-fix behave identically.
// ===========================================================================
BEZEL_TEST(explicit_type_arg_drives_inference_4935,
           t2_untyped_lambda_with_explicit_int_nullable_runs_spec_14_5) {
    const char* prog =
        "module M; "
        "public class Util "
        "  public static method Map<T>(v: T, f: (T) -> string): string "
        "    return f(v); "
        "  end Map; "
        "end Util; "
        "public class P "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Map<int?>(5, lambda(y) => \"n\")); "
        "    return 0; "
        "  end Main; "
        "end P;";
    {
        auto r = runAot(prog);
        BEZEL_EXPECT_FALSE(hasAnyError(r));
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("n\n"));
    }
    {
        auto r = runAotChecked(prog);
        BEZEL_EXPECT_FALSE(hasAnyError(r));
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("n\n"));
    }
}

// ===========================================================================
// T3 (hard constraint — main's condition 1):
// `Util.Merge<int>(5, true)` — second arg `true` (bool) is NOT implicitly
// convertible to the substituted formal `int` (§5.1).
//
// Pre-fix REJECT (via the spurious "inconsistent type-argument
// inference" diagnostic — the wrong reason). Post-fix REJECT via the
// correct §5.1 non-convertibility diagnostic. Same behaviour on both
// sides (REJECT), but the diagnostic text changes — main's condition 3
// flags this as a behaviour flip for which we MUST update every lit pin.
// If post-fix ACCEPTed, this is a HARD REJECT (unsound — `bool` as `int`).
// ===========================================================================
BEZEL_TEST(explicit_type_arg_drives_inference_4935,
           t3_non_implicit_convertible_explicit_arg_still_rejected_spec_5_1) {
    const char* prog =
        "module M; "
        "public class Util "
        "  public static method Merge<T>(x: T, y: T): T "
        "    return x; "
        "  end Merge; "
        "end Util; "
        "public class P "
        "  public static method Main(): int "
        "    var v := Util.Merge<int>(5, true); "
        "    Console.WriteLine(\"should-not-print\"); "
        "    return 0; "
        "  end Main; "
        "end P;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The wrong-reason diagnostic must NOT fire post-fix (the inference
    // loop is gated off; argSubst is empty; inferConflicts is empty).
    BEZEL_EXPECT_FALSE(has896Diag(r));
    // The §5.1 non-convertibility diagnostic MUST fire post-fix (the
    // later arg-vs-param check on the substituted formal catches it).
    BEZEL_EXPECT_TRUE(has51Diag(r));
}

// ===========================================================================
// T4 (#4895 homogeneous-empty control — main's condition 4):
// `Util.Show<int>([])` — explicit T = int, empty literal. The literal
// has no elements to disagree, so it determinately binds nothing — the
// explicit `<int>` supplies T.
//
// Pre-fix and post-fix both ACCEPT (the #4895 safety net at L5151-5213
// fires only for heterogeneous literals, not empty ones). This verifies
// main's condition 4: the #4895 safety net consumes argSubst
// (originally built by the inference loop), but with explicit args T is
// already supplied, so the safety net sees a fully-bound set and the
// empty literal is silently allowed. Behaviour unchanged pre/post.
// ===========================================================================
BEZEL_TEST(explicit_type_arg_drives_inference_4935,
           t4_explicit_type_arg_with_empty_literal_unchanged_spec_14_5_15_4) {
    const char* prog =
        "module M; "
        "public class Util "
        "  public static method Show<T>(items: Array<T>): int "
        "    return 0; "
        "  end Show; "
        "end Util; "
        "public class P "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Show<int>([])); "
        "    return 0; "
        "  end Main; "
        "end P;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(hasAnyError(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// T5 (#896 case preserved — main's condition 3):
// `Util.Merge(x: int?, y: bool?)` — NO explicit type argument, args have
// incompatible inferred types (`int?` and `bool?`). The genuine #896 case.
//
// Pre-fix and post-fix both REJECT (the inference loop still runs when
// there are no explicit args — the `!hadExplicitTypeArgs` gate is
// precisely the gate that distinguishes this from T3). The diagnostic
// remains the §14.1/#896 "inconsistent type-argument inference" text.
// ===========================================================================
BEZEL_TEST(explicit_type_arg_drives_inference_4935,
           t5_no_explicit_args_genuine_conflict_still_rejected_spec_14_1) {
    const char* prog =
        "module M; "
        "public class Util "
        "  public static method Merge<T>(x: T, y: T): T "
        "    return x; "
        "  end Merge; "
        "end Util; "
        "public class P "
        "  public static method Main(): int "
        "    var v := Util.Merge(5 as int?, true); "
        "    return 0; "
        "  end Main; "
        "end P;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(has896Diag(r));
}

// ===========================================================================
// T6 (heterogeneous literal with explicit type arg — control):
// `Util.Map<int?>([1, "a"], lambda(y) => "n")` — explicit T = int?, but
// the literal `[1, "a"]` is heterogeneous (int + string). The literal's
// elements must satisfy the substituted parameter type `Array<int?>`.
//
// Pre-fix REJECT (the spurious "inconsistent type-argument inference"
// caught the literal's int vs string mismatch against T). Post-fix REJECT
// via the §5.1 collection-literal element non-convertibility check
// (the literal `[1, "a"]` cannot determine a single `int?` element
// type). Both sides REJECT — same observable behaviour (call rejected),
// different diagnostic text. The brief's "ACCEPT" label was a typo;
// the row pins REJECT pre/post.
//
// Note: the #4895 safety net is itself gated on `!hadExplicitTypeArgs`
// (L5096) and so does NOT fire in this case — the §5.1 element check
// runs earlier and catches it. This is sound: with explicit T already
// supplied, the literal's heterogeneity is a §5.1 question (each
// element must be assignable to the substituted `int?`), not a §14.5
// inference question.
// ===========================================================================
BEZEL_TEST(explicit_type_arg_drives_inference_4935,
           t6_heterogeneous_literal_with_explicit_arg_still_rejected_spec_14_5) {
    const char* prog =
        "module M; "
        "public class Util "
        "  public static method Map<T>(v: Array<T>, f: (T) -> string): string "
        "    return f(v[0]); "
        "  end Map; "
        "end Util; "
        "public class P "
        "  public static method Main(): int "
        "    var v := Util.Map<int?>([1, \"a\"], lambda(y) => \"n\"); "
        "    return 0; "
        "  end Main; "
        "end P;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The wrong-reason "inconsistent type-argument inference" diagnostic
    // must NOT fire post-fix (inference loop is gated off; argSubst is
    // empty; inferConflicts is empty). Pre-fix fired it; post-fix does not.
    BEZEL_EXPECT_FALSE(has896Diag(r));
    // The #4895 safety net does not fire in this case (it is itself gated
    // on `!hadExplicitTypeArgs`), so the §5.1 element check is what
    // catches it. Both pre and post REJECT, just with different text.
    BEZEL_EXPECT_FALSE(has4895Diag(r));
    // Either §5.1 (post-fix) or some other correct diagnostic — at minimum
    // it is NOT the spurious #896 text.
    BEZEL_EXPECT_TRUE(hasAnyError(r));
}
