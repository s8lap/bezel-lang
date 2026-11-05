// #6254 (§14.5, §22.6, v0.355) — a LONE generic candidate whose type
// parameter only a BARE `null` would bind is INAPPLICABLE.
//
// WHAT WAS BROKEN. §14.5 phase 1: "Each argument whose type is
// self-determined ... is unified structurally against its parameter type (a
// bare `null` literal argument is not self-determined and contributes no
// binding)". §14.5 "Interaction with overloading": "a candidate whose
// inference fails is inapplicable and drops out (the call then resolves among
// the remaining candidates as usual)". With ONE candidate, none survives, so
// §22.6 stage 3 makes the call an error.
//
// MEASURED on this tree before the fix (bezelc from a clean HEAD):
//   * `M<T>(x: T?)` called as `S.M(null)` produced MALFORMED IR:
//     "Call parameter type does not match function signature! ... call i32
//     @\"bz$M.S$M\"(ptr null)" then "bezelc: codegen error: IR verification
//     failed" (rc=1) — the call was ACCEPTED by sema with `T` never bound.
//   * `M<T>(x: Array<T>?)` called as `S.M(null)` compiled and RAN (printing
//     `1`) with `T` never bound.
//   * the void-returning twin `M<T>(x: T?)` produced the same malformed IR.
//
// §14.5 phase 3 bounds what a TARGET context can do: "A type parameter still
// unbound is bound from the call's target type ... by unifying the method's
// declared return type — current bindings applied — against the target". A
// target can therefore supply the parameter ONLY through the method's
// DECLARED RETURN TYPE, so a parameter the return type does not mention can
// never be completed — the explicit type argument is the only remedy.
//
// The EVIDENCE the fix requires is CERTAIN, because an empty substitution
// map is not by itself evidence of a failed match: `unifyArgSubst` binds only
// on a structural head match and never through a §12 subtype step, so a VALID
// call (`Array<int>` against `S<V>(x: IEnumerable<V>)`) also leaves the map
// empty (see the #6334 lane's measurement, and the control row below). The
// certain evidence is §14.5's own sentence: the parameter is unbound only
// because the argument at its position is a bare `null` literal, which
// contributes no binding.
//
// Every row is on `runAotChecked`, the ASan-checked AOT oracle. The reject
// rows must NOT run; the admit rows must run and print a value read out of
// the arguments, so a row whose body is a constant could not tell a working
// call from a mis-dispatched one.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// The §14.5 diagnostic this fix emits. The leading phrase is the existing
// inference-failure wording (`typeCall`'s #4183 check opens the same way);
// the tail names the bare-`null` reason.
const char* const kUninferableNeedle =
    "cannot infer the type argument `T` of `M`'s call — a bare `null` "
    "literal argument is not self-determined and contributes no binding";

// The pre-existing §14.5 diagnostic for the RETURN-TYPE shape, which this fix
// must NOT replace and must not double-report.
const char* const kExistingUninferableNeedle =
    "mentioned only inside a parameter whose declared type did not unify";

// The §22.6 diagnostic the overload-set shape keeps (a generic candidate
// beside a non-generic one that a `null` does NOT fit).
const char* const kNoOverloadNeedle = "no overload of `M` is applicable";

// Assert the call is REJECTED, and rejected with THIS fix's diagnostic — not
// by crashing, and not by some unrelated error.
void expectRejected(const std::string& body, const char* needle) {
    auto r = runAotChecked("module M;\n" + body);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

// Assert the call still COMPILES and runs, printing the expected value.
void expectRuns(const std::string& body, const std::string& expected) {
    auto r = runAotChecked("module M;\n" + body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// s14_5_0101: THE ISSUE'S REPRO. A lone `M<T>(x: T?)` given a bare `null`.
// No argument binds `T` (the bare `null` is not self-determined) and the
// declared return type (`int`) does not mention `T`, so no target can supply
// it either: the candidate's inference FAILED, the candidate is inapplicable,
// and with one candidate the call is an error.
BEZEL_TEST(generic_bare_null_inference_6254,
           lone_generic_t_optional_rejects_bare_null_spec_14_5_22_6) {
    expectRejected(
        "static class S\n"
        "  public static method M<T>(x: T?): int => 1;\n"
        "end S;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(S.M(null));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        kUninferableNeedle);
}

// s14_5_0102: THE ISSUE'S SECOND REPRO. `M<T>(x: Array<T>?)` given a bare
// `null`: before the fix this compiled and RAN with `T` never bound. The
// constructed parameter type mentions `T`, so the same §14.5 phase-1 sentence
// applies.
BEZEL_TEST(generic_bare_null_inference_6254,
           lone_generic_array_t_optional_rejects_bare_null_spec_14_5) {
    expectRejected(
        "static class S\n"
        "  public static method M<T>(x: Array<T>?): int => 1;\n"
        "end S;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(S.M(null));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        kUninferableNeedle);
}

// s14_5_0103: the VOID twin. A void-returning generic method has no return
// type for a target to unify against, so the parameter is unbound for the
// same reason — and before the fix this reached codegen and produced the
// same malformed IR ("Call parameter type does not match function
// signature!").
BEZEL_TEST(generic_bare_null_inference_6254,
           lone_generic_void_candidate_rejects_bare_null_spec_14_5) {
    expectRejected(
        "static class S\n"
        "  public static method M<T>(x: T?)\n"
        "  end M;\n"
        "end S;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    S.M(null);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        kUninferableNeedle);
}

// s14_5_0104: THE EXPLICIT REMEDY (§14.5 "Explicit lists are all-or-nothing":
// "When a full explicit list is supplied, no inference runs"). `S.M<int>(null)`
// binds every parameter and must still compile and run.
BEZEL_TEST(generic_bare_null_inference_6254,
           explicit_type_argument_null_still_compiles_spec_14_5) {
    expectRuns(
        "static class S\n"
        "  public static method M<T>(x: T?): int => 41;\n"
        "end S;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(S.M<int>(null));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "41\n");
}

// s14_5_0105: CONTROL — a SECOND argument binds the parameter, so the bare
// `null` at the first position does not leave it unbound. `M<T>(x: T?, y: T)`
// called as `S.M(null, 40)` infers `T = int` and must compile and run. This
// is the row that fails on a fix that rejects every call carrying a `null`.
BEZEL_TEST(generic_bare_null_inference_6254,
           second_argument_binds_t_still_compiles_spec_14_5) {
    expectRuns(
        "static class S\n"
        "  public static method M<T>(x: T?, y: T): int => 42;\n"
        "end S;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(S.M(null, 40));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "42\n");
}

// s14_5_0106: CONTROL — a `null`-TYPED VALUE is not a bare `null` literal.
// §14.5 phase 1 makes only the LITERAL not self-determined: "an argument of
// type `U?` binds `X` to `U`". `n: int?` therefore binds `T = int` and the
// call must compile and run. This is the row that fails on a fix that treats
// every nullable argument as a bare `null`.
BEZEL_TEST(generic_bare_null_inference_6254,
           null_typed_value_binds_t_still_compiles_spec_14_5) {
    expectRuns(
        "static class S\n"
        "  public static method M<T>(x: T?): int => 43;\n"
        "end S;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var n: int? := null;\n"
        "    Console.WriteLine(S.M(n));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "43\n");
}

// s14_5_0107: CONTROL — an overload set in which a NON-GENERIC candidate
// accepts the `null`. §22.6's non-generic preference (§14.5 "Interaction
// with overloading") keeps the non-generic candidate, so the call compiles
// and prints ITS body — the generic candidate simply drops out.
BEZEL_TEST(generic_bare_null_inference_6254,
           overload_set_with_non_generic_null_candidate_compiles_spec_22_6) {
    expectRuns(
        "static class S\n"
        "  public static method M<T>(x: T?): int => 1;\n"
        "  public static method M(x: string?): int => 2;\n"
        "end S;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(S.M(null));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "2\n");
}

// s14_5_0108: CONTROL — the overload-set shape whose diagnostic this fix
// must NOT change: a generic candidate beside a non-generic one that the
// `null` does NOT fit (`string` is not nullable). The call stays rejected,
// with the §22.6 diagnostic it has always had.
BEZEL_TEST(generic_bare_null_inference_6254,
           overload_set_generic_only_null_keeps_section_22_6_diagnostic) {
    expectRejected(
        "static class S\n"
        "  public static method M<T>(x: T?): int => 1;\n"
        "  public static method M(x: string): int => 2;\n"
        "end S;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(S.M(null));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        kNoOverloadNeedle);
}

// s14_5_0109: CONTROL — the §12 SUBTYPE step. `Array<int>` reaches
// `IEnumerable<int>`, so `S<V>(x: IEnumerable<V>)` is applicable and the
// parameter is bound even though `unifyArgSubst` does not model the subtype
// step. This is the row that fails on a fix keyed on "the substitution map
// is empty" instead of on "the only argument is a bare `null`".
BEZEL_TEST(generic_bare_null_inference_6254,
           subtype_step_to_enumerable_still_compiles_spec_12_1_14_5) {
    expectRuns(
        "static class S\n"
        "  public static method M<T>(x: IEnumerable<T>): int => 1;\n"
        "end S;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    using var a := new Array<int>();\n"
        "    a.Add(6);\n"
        "    Console.WriteLine(S.M(a) + a.Count);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "2\n");
}

// s14_5_0110: CONTROL — the RETURN-TYPE shape keeps its own, pre-existing
// §14.5 diagnostic. `M<T>(x: T?): T` mentions `T` in the declared return
// type, so a target context can complete it (§14.5 phase 3) and the
// pre-existing check owns the shape. This fix must neither replace that
// diagnostic nor add a second message beside it.
BEZEL_TEST(generic_bare_null_inference_6254,
           return_type_mentions_t_keeps_the_existing_diagnostic_spec_14_5) {
    auto r = runAotChecked(
        "module M;\n"
        "static class S\n"
        "  public static method M<T>(x: T?): T => x;\n"
        "end S;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(S.M(null));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kExistingUninferableNeedle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kUninferableNeedle));
}
