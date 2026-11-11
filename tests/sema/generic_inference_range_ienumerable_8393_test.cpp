// fix #8393 (§14.5 phase 1, §5.1, stdlib §9.8, v0.357): a generic method's
// type-argument inference must bind `T` through a `Range<int>` argument's
// IMPLEMENTED interface, and the §5.1 conversion that follows must accept it.
//
// THE SPEC.
//
//   * §14.5 *Type-argument inference for generic methods*, phase 1
//     *Argument-driven unification* (v0.357): "a constructed parameter type
//     `G<P1, …, Pn>` unifies component-wise and **invariantly** against an
//     argument of the same generic type, or against **the unique
//     instantiation of `G` that the argument's type implements or extends**
//     (an `Array<int>` argument against `xs: IEnumerable<T>` binds `T` to
//     `int`); an argument whose type reaches no instantiation of `G` makes
//     the candidate inapplicable". §14.5 closes the phase with: "Conversions
//     get their say afterward, argument by argument, at the §22.6
//     applicability check **against the substituted signature**".
//   * stdlib §9.8 *`Range<T>`* (v0.357): "public record Range<T>:
//     IEnumerable<T>" — a `Range<int>` DOES implement `IEnumerable<int>`, so
//     `Range<int>` is the `Range<T>` twin of the spec's own `Array<int>`
//     example.
//
// THE DEFECT. Three gaps, all in `src/sema/ExprTypecheck.cpp`:
//
//   1. The §26.5 builtin-conformance table (`builtinInterfaceAncestors`) had
//      no `Range` row, so the walk never reached `IEnumerable<T>` for a
//      range.
//   2. `collectBuiltinAncestorInstantiationsOf` — the collector that answers
//      "which instantiation of `IEnumerable` does this argument's type
//      reach?" — bailed out at its first guard whenever the argument's
//      TypeRef carried ANY declaration. A `Range<int>` TypeRef carries
//      `declaration = builtinClassSym("Range")` (a compiler-intrinsic stub
//      with no TypeDecl and no module — the same shape #8103 fixed for
//      `FrozenArray<int>.From(a)` one level down), so the guard threw the
//      argument away before the table was ever consulted.
//   3. With the inference fixed, `T` bound to `int` — but the per-argument
//      §5.1 check still rejected the call, because it substitutes the
//      callee's EXPLICIT type arguments into the parameter type and not the
//      INFERRED ones, so it saw `IEnumerable<T>` (unsubstituted) against
//      the `Range<int>` argument's element type `int`. The #6998
//      `Range<T>`→`IEnumerable<T>` arm of `checkAssignable` demanded
//      element-type IDENTITY; the general machinery
//      (`variantGenericAssignable`'s per-argument loop) has always skipped a
//      FREE type variable on either side, which is why the `Array<int>` twin
//      of this very call was admitted all along.
//
// MEASURED on the pre-fix base (r30-c1, 71317e6c9): the issue's own repro
// was REJECTED with
//
//   "the argument `xs` to call to `CountIf` has a type that is not
//    implicitly convertible to the target type (§5.1)"
//
// — the generic §5.1 wording, with no mention of the inference that failed.
// `CountIf<int>(1..10, …)` — the SAME call with the type argument WRITTEN —
// compiled, which is the asymmetry the issue names.
//
// The rows below pin the repaired verdicts and the MUST-STAY-REJECTED /
// MUST-STAY-GREEN controls around them: a `Range<int>` implements ONLY
// `IEnumerable<T>` (stdlib §9.8), so it must still be rejected at
// `IReadOnlyList<T>`, at `IList<T>`, at `ICollection<T>`, and at an
// `IEnumerable<string>` — and an explicitly written `IEnumerable<string>`
// type argument must still beat the inference.
//
// All rows are on the `runAot` oracle (the in-process JIT harness is
// deprecated).

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"        // runAot / runAotOut
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// §5.1's wording for a rejected conversion, pinned as a whole so a reworded
// diagnostic is a visible change rather than a silent one.
const char* kNotConvertible =
    "has a type that is not implicitly convertible to the target type (§5.1)";

// The rejection oracle: the rejection AND the wording. `r.ran == false` alone
// is satisfied by a crash, a signal, a timeout, an ASan report and an
// IR-verification failure alike, so it discriminates nothing on its own.
bool rejectedWith(const JitResult& r, const char* msg) {
    return failedAtCompileTimeWithRealLocation(r) &&
           diagnosticMessageContains(r, msg);
}

// The issue's EXACT repro program (§14.5 phase 1 / stdlib §9.8). The type
// argument is INFERRED; the expected result is `5` — the count of the even
// numbers in `1..10`.
std::string issueRepro() {
    return "module P;\n"
           "public class Program\n"
           "  private static method CountIf<T>(xs: IEnumerable<T>, "
           "pred: (T) -> bool): int\n"
           "    var n := 0;\n"
           "    for var x in xs do\n"
           "      if pred(x) then n++; end;\n"
           "    end;\n"
           "    return n;\n"
           "  end CountIf;\n"
           "  public static method Main()\n"
           "    Console.WriteLine(CountIf(1..10, lambda(n) => n mod 2 = 0));\n"
           "  end Main;\n"
           "end Program;\n";
}

// The same call with the type argument WRITTEN — the shape the issue names as
// working (`CountIf<int>(…)` compiles), and the twin the inference must
// agree with.
std::string issueReproExplicit() {
    return "module P;\n"
           "public class Program\n"
           "  private static method CountIf<T>(xs: IEnumerable<T>, "
           "pred: (T) -> bool): int\n"
           "    var n := 0;\n"
           "    for var x in xs do\n"
           "      if pred(x) then n++; end;\n"
           "    end;\n"
           "    return n;\n"
           "  end CountIf;\n"
           "  public static method Main()\n"
           "    Console.WriteLine(CountIf<int>(1..10, lambda(n) => n mod 2 = 0));\n"
           "  end Main;\n"
           "end Program;\n";
}

// A program whose generic method RETURNS the inferred `T`, so the call site's
// binding is observable: `var v: int := First(1..10);` type-checks only if
// `T` was inferred as `int` (an unbound `T` leaves the return type the bare
// type parameter, which §14.5 rejects with "cannot infer the type argument
// `T`").
std::string inferenceProbe() {
    return "module P;\n"
           "public class Program\n"
           "  private static method First<T>(xs: IEnumerable<T>): T\n"
           "    for var x in xs do\n"
           "      return x;\n"
           "    end;\n"
           "    return default(T);\n"
           "  end First;\n"
           "  public static method Main()\n"
           "    var v: int := First(1..10);\n"
           "    Console.WriteLine(v);\n"
           "  end Main;\n"
           "end Program;\n";
}

// The `Array<int>` twin of the issue's call — the spec's own worked example
// ("an `Array<int>` argument against `xs: IEnumerable<T>` binds `T` to
// `int`"). It was admitted before this fix and must stay admitted.
std::string arrayTwin() {
    return "module P;\n"
           "public class Program\n"
           "  private static method CountIf<T>(xs: IEnumerable<T>, "
           "pred: (T) -> bool): int\n"
           "    var n := 0;\n"
           "    for var x in xs do\n"
           "      if pred(x) then n++; end;\n"
           "    end;\n"
           "    return n;\n"
           "  end CountIf;\n"
           "  public static method Main()\n"
           "    using var a: Array<int> := [1, 2, 3, 4];\n"
           "    Console.WriteLine(CountIf(a, lambda(n) => n mod 2 = 0));\n"
           "  end Main;\n"
           "end Program;\n";
}

// A program that passes a `Range<int>` at a parameter whose declared type is
// `PARAM`. Used for the MUST-STAY-REJECTED rows. `callTypeArgs` is the type
// argument list written at the CALL SITE (empty = inferred).
std::string rangeAtParam(const std::string& param,
                         const std::string& callTypeArgs = "") {
    return "module P;\n"
           "public class Program\n"
           "  private static method Take(xs: " + param + "): int\n"
           "    var n := 0;\n"
           "    for var x in xs do\n"
           "      n++;\n"
           "    end;\n"
           "    return n;\n"
           "  end Take;\n"
           "  public static method Main()\n"
           "    Console.WriteLine(Take" + callTypeArgs + "(1..10));\n"
           "  end Main;\n"
           "end Program;\n";
}

// The same, but the parameter's declared type is a CONSTRUCTED type carrying a
// method type parameter `T` (so the call's type argument is INFERRED).
std::string rangeAtInferredParam(const std::string& param) {
    return "module P;\n"
           "public class Program\n"
           "  private static method Take<T>(xs: " + param + "): int\n"
           "    var n := 0;\n"
           "    for var x in xs do\n"
           "      n++;\n"
           "    end;\n"
           "    return n;\n"
           "  end Take;\n"
           "  public static method Main()\n"
           "    Console.WriteLine(Take(1..10));\n"
           "  end Main;\n"
           "end Program;\n";
}

// The same, but the call writes its type argument explicitly: `Take<ARGS>`.
std::string rangeAtExplicitTypeArg(const std::string& param,
                                   const std::string& typeArgs) {
    return "module P;\n"
           "public class Program\n"
           "  private static method Take<T>(xs: " + param + "): int\n"
           "    var n := 0;\n"
           "    for var x in xs do\n"
           "      n++;\n"
           "    end;\n"
           "    return n;\n"
           "  end Take;\n"
           "  public static method Main()\n"
           "    Console.WriteLine(Take" + typeArgs + "(1..10));\n"
           "  end Main;\n"
           "end Program;\n";
}

// A USER extension block over `IEnumerable<T>`, called on a range receiver.
std::string userExtensionOnRange() {
    return "module P;\n"
           "extension IEnumerable<T>\n"
           "  public method R42Tag(): int => 7;\n"
           "end;\n"
           "public class Program\n"
           "  public static method Main()\n"
           "    Console.WriteLine((1..10).R42Tag());\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

// ===========================================================================
// T1 — the issue's own repro: the inference binds `T` and the call compiles.
// ===========================================================================

// §14.5 phase 1 / stdlib §9.8 (v0.357): a `Range<int>` argument reaches the
// unique instantiation of `IEnumerable` it implements, so `T` binds to `int`
// and the count of the even numbers in `1..10` is `5`.
//
// PRE-FIX this program was REJECTED with §5.1's generic "not implicitly
// convertible" wording; `runAotOut`'s "<did-not-run>" sentinel proves both
// halves at once — the program is now ACCEPTED and prints `5`.
BEZEL_TEST(sema_generic_inference_range_ienumerable_8393,
           t1_issue_repro_infers_t_and_prints_5_spec_14_5) {
    BEZEL_EXPECT_EQ(runAotOut(issueRepro()), std::string("5"));
}

// The same call with the type argument WRITTEN (`CountIf<int>(…)`) — the shape
// the issue names as the working twin. It must print the SAME `5`: the
// inferred call and the written call are one call.
BEZEL_TEST(sema_generic_inference_range_ienumerable_8393,
           t1_explicit_type_argument_twin_still_prints_5_spec_14_5) {
    BEZEL_EXPECT_EQ(runAotOut(issueReproExplicit()), std::string("5"));
}

// The binding itself, observed: the method RETURNS the inferred `T`, so
// `var v: int := First(1..10);` type-checks only when `T` was inferred as
// `int`. An unbound `T` leaves the bare type parameter in the return type
// and §14.5 rejects the call with "cannot infer the type argument `T`".
BEZEL_TEST(sema_generic_inference_range_ienumerable_8393,
           t1_inferred_t_is_int_spec_14_5) {
    BEZEL_EXPECT_EQ(runAotOut(inferenceProbe()), std::string("1"));
}

// ===========================================================================
// T2 — MUST-STAY-GREEN: the `Array<int>` twin of the same call.
// ===========================================================================

// §14.5 phase 1's own worked example ("an `Array<int>` argument against
// `xs: IEnumerable<T>` binds `T` to `int`") was admitted before this fix and
// must stay admitted: the `Range<int>` repair must not have narrowed it.
BEZEL_TEST(sema_generic_inference_range_ienumerable_8393,
           t2_array_int_twin_still_binds_t_spec_14_5) {
    BEZEL_EXPECT_EQ(runAotOut(arrayTwin()), std::string("2"));
}

// ===========================================================================
// T3 — MUST-STAY-REJECTED: `Range<T>` implements ONLY `IEnumerable<T>`.
// ===========================================================================

// stdlib §9.8 (v0.357): `public record Range<T>: IEnumerable<T>` — and
// nothing else. So a `Range<int>` has no §5.1 row to `IReadOnlyList<T>` and
// the call is rejected. (The same rejection the pre-fix base gave, for the
// same reason.)
BEZEL_TEST(sema_generic_inference_range_ienumerable_8393,
           t3_range_at_ireadonlylist_is_rejected_spec_9_8) {
    auto r = runAot(rangeAtParam("IReadOnlyList<int>"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// stdlib §9.8: no `IList<T>` either.
BEZEL_TEST(sema_generic_inference_range_ienumerable_8393,
           t3_range_at_ilist_is_rejected_spec_9_8) {
    auto r = runAot(rangeAtParam("IList<int>"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// stdlib §9.8: no `ICollection<T>` either.
BEZEL_TEST(sema_generic_inference_range_ienumerable_8393,
           t3_range_at_icollection_is_rejected_spec_9_8) {
    auto r = runAot(rangeAtParam("ICollection<int>"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// A `Range<int>` does not convert to an `IEnumerable<string>`: the element
// types differ (§5.1 / §12.9 — a record is a value type, so no variance
// applies).
BEZEL_TEST(sema_generic_inference_range_ienumerable_8393,
           t3_range_at_ienumerable_string_is_rejected_spec_5_1) {
    auto r = runAot(rangeAtParam("IEnumerable<string>"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// An EXPLICITLY WRITTEN type argument is not overridden by the inference:
// `Take<string>(1..10)` at `xs: IEnumerable<string>` is rejected — §14.5's
// "Explicit lists are all-or-nothing … When a full explicit list is
// supplied, no inference runs."
BEZEL_TEST(sema_generic_inference_range_ienumerable_8393,
           t3_explicit_string_type_argument_is_rejected_spec_14_5) {
    auto r = runAot(rangeAtExplicitTypeArg("IEnumerable<T>", "<string>"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// The inference must not make a `Range<int>` convertible to an interface it
// does not implement — the `IList<T>` shape with the INFERRED type argument
// (`xs: IList<T>`): `Range<int>` implements no `IList` at all, so the
// candidate reaches no instantiation of `IList` and §14.5 makes it
// inapplicable.
BEZEL_TEST(sema_generic_inference_range_ienumerable_8393,
           t3_range_at_inferred_ilist_is_rejected_spec_14_5) {
    auto r = runAot(rangeAtInferredParam("IList<T>"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// ===========================================================================
// T4 — the conversion at a CONCRETE `IEnumerable<int>` parameter.
// ===========================================================================

// The #6998 / #7029 shape: `Range<int>` at a CONCRETE `IEnumerable<int>`
// parameter converts (a method argument is one of the admitted positions).
// The inference repair must not have disturbed it.
BEZEL_TEST(sema_generic_inference_range_ienumerable_8393,
           t4_range_at_concrete_ienumerable_int_is_accepted_spec_9_8) {
    BEZEL_EXPECT_EQ(runAotOut(rangeAtParam("IEnumerable<int>")), std::string("10"));
}

// ===========================================================================
// T5 — MUST-STAY-REJECTED: a range is not an EXTENSION receiver.
// ===========================================================================

// §16.3 / #7029 (v0.357): the `Range<T>` → `IEnumerable<T>` conversion is
// admitted only where codegen's owning enumerator frame has a disposal edge —
// a local declaration's initializer, a method or constructor argument, or a
// returned value. An extension RECEIVER is none of those, so a user
// `extension IEnumerable<T>` block must not be matched to a range receiver.
//
// MEASURED: with the §26.5 table's `Range` row and no gate on the extension
// walk, this program COMPILED, printed NOTHING, and emitted only an internal
// "failed to lower to a value" warning — a silent wrong answer where the
// pre-fix compiler gave a clean refusal. The refusal must survive the fix.
BEZEL_TEST(sema_generic_inference_range_ienumerable_8393,
           t5_range_is_not_an_extension_receiver_spec_16_3) {
    auto r = runAot(userExtensionOnRange());
    BEZEL_EXPECT_FALSE(r.ran);
    // A COMPILE-TIME refusal at a real source location — not a crash, a
    // timeout or an IR-verification failure.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "Range"));
    // The program must not have been accepted silently: no "produced no
    // value" degradation may have let it through as a compile.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "failed to lower"));
}
