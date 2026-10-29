// Spec-derived regression test for issue #3425 — the `new T(...)` twin of
// #3398.
//
// spec §22.5 "Optional parameters and named arguments" (v0.283); spec §17.7
// "The `new` operator" (v0.283); stdlib §2.5, §3.1, §3.3, §4.2, §31.4.
//
// WHAT THE DEFECT WAS. §22.5's argument binder ran for a user-declared callee
// and, since #3398, for a built-in MEMBER call. A built-in type's constructors
// are arms of codegen's `lowerNew`, not `ConstructorSymbol`s sema can see, so
// no binder ever ran over `new T(...)` on a built-in: the argument NAME was
// discarded outright and every argument bound POSITIONALLY.
//
// MEASURED ON `ef339fa` (build/bezelc, real AOT compile + native run, exit 0
// on every line):
//
//   new StringBuilder(zzznotaparam := "ab")   -> builds "ab"  (NONSENSE name
//                                                 accepted; stdlib §3.1
//                                                 declares `initial`)
//   new Array<int>(3, 7)                      -> Count=3, [0]=7  (correct)
//   new Array<int>(size := 3, fill := 7)      -> Count=3, [0]=7  (correct)
//   new Array<int>(fill := 7, size := 3)      -> Count=7, [0]=3  <-- WRONG
//                                                 VALUE: bound positionally as
//                                                 Array<int>(7, 3), i.e. SEVEN
//                                                 slots of 3 where §22.5
//                                                 requires three slots of 7.
//
// The last row is the load-bearing one, and it refutes #3425's own severity
// note ("lower than #3398's … the common built-in constructors take one
// [parameter]"). `Array<T>(size, fill)` (stdlib §4.2),
// `Semaphore(initialCount, maxCount)` (§6.3), `Dictionary<K,V>(capacity,
// comparer)` (§4.5), `DateTime(year, month, day)` (§2.5) and
// `ArgumentException(message, paramName)` (spec §31.4) all take two or more,
// so this is a silent WRONG VALUE — and, for the `Array` case, a wrong
// allocation size — not merely a false accept of nonsense names.
//
// WHAT THE FIX DOES. `common::builtinCtorParamNameCandidates` carries the
// declared constructor parameter names, and `ExprTC::checkBuiltinCtorNamedArgs`
// feeds them to the SAME `bindArgsToParams` the user, imported and built-in
// member paths use, so the four §22.5 violations (unknown name, bound twice,
// positional-after-named, required-not-supplied) draw the user path's own
// diagnostics. The two implementation-limitation rejections #3398 states hold
// here for the same reasons and are LOUD:
//
//   1. A named argument on a built-in constructor the registry carries no
//      names for is REJECTED (rejecting an unknown name needs a COMPLETE
//      list; a partial table must otherwise fall back to the silent
//      positional binding this fix exists to remove).
//   2. A named argument that binds to a parameter OTHER than its own position
//      is REJECTED wherever codegen lowers the construction by `ne.args[i]`
//      POSITION, because §22.5's *Evaluation order* rule forbids buying the
//      reorder by permuting the argument list ("A call's supplied argument
//      expressions are evaluated in call-site textual order"). Lifting it is
//      #3423's codegen work. The two record-shaped builtins whose `lowerNew`
//      arm already resolves each argument to its field slot BY NAME (#3098 —
//      `ExceptionInfo`, `ReadOnlyStringSpan`) are exempt and keep accepting
//      the reorder; that exemption has its own row below.
//
// FAIL-WITHOUT / PASS-WITH. Every `rejects_*` row compiles CLEAN on `ef339fa`
// — the diagnostic text it asserts does not exist on that tree at all — and
// every `accepts_*` / value row holds on both trees, so the accept rows are
// the false-rejection guard rather than a restatement of the fix. The
// `exceptioninfo_*` and `user_declared_*` rows are the branch-order guard:
// adding an arm to a classify-and-report path can re-route other decisions,
// so they assert the two constructions that ALREADY bound by name still do.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A whole program whose `Main` body is `body`. Observables travel on stdout,
// never on the exit status: a process exit status is masked to 8 bits
// (`WEXITSTATUS`) and these observables are strings and composite integers.
std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// True iff SOME diagnostic contains `sub`. Rejection rows assert on SPECIFIC
// message text, never on the coarse `hasErrors()` — `runAot` synthesizes an
// Error diagnostic for a runtime SIGSEGV/ICE too, so `hasErrors()` cannot
// distinguish a genuine reject from a crash.
bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

// The #3425 limitation diagnostics, anchored on the issue number so a future
// §22.5 diagnostic cannot satisfy these rows by accident.
constexpr const char* kReorderDiag =
    "a named argument that binds to a parameter other than its own position "
    "is not supported on a built-in constructor";
constexpr const char* kNoNamesDiag =
    "named arguments are not supported on this built-in constructor";
constexpr const char* kNoCandidateDiag =
    "the named arguments match none of the declared overloads";

// The SHARED §22.5 binder's own strings — the ones a user-declared callee
// draws. Asserting these is what pins "same binder, not a second copy".
constexpr const char* kUnknownNameDiag = "has no parameter named";
constexpr const char* kBoundTwiceDiag = "is bound more than once";
constexpr const char* kPositionalAfterNamedDiag =
    "a positional argument may not follow a named argument";

}  // namespace

// ---------------------------------------------------------------------------
// The issue's repro — and the WRONG-VALUE row it understated.
// ---------------------------------------------------------------------------

// fix #3423 (§22.5, v0.283): the reordered named construction is now
// accepted and evaluates correctly. The two arguments are DISTINGUISHABLE, so
// a positional mis-bind is visible in the answer: on `ef339fa` this compiled
// and printed 7 then 3 — an array of SEVEN slots each holding 3 — where
// §22.5's binding gives the correct three slots of 7.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           accepts_reordered_named_args_on_builtin_array_constructor) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var a := new Array<int>(fill := 7, size := 3); "
            "    Console.WriteLine(a.Count); "
            "    Console.WriteLine(a[0]); ")),
        std::string("3\n7"));
}

// #3425 / §22.5 / stdlib §4.2: the SAME construction written in declaration
// order computes the declared value, and matches the character-identical
// positional spelling. This is the distinguishable-value oracle for the row
// above: 3 and 7 are different numbers, so `Count` and `[0]` cannot both be
// right under a positional mis-bind.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           accepts_in_order_named_args_on_builtin_array_constructor) {
    const std::string named = runAotOut(prog(
        "    var a := new Array<int>(size := 3, fill := 7); "
        "    Console.WriteLine(a.Count); "
        "    Console.WriteLine(a[0]); "));
    const std::string positional = runAotOut(prog(
        "    var a := new Array<int>(3, 7); "
        "    Console.WriteLine(a.Count); "
        "    Console.WriteLine(a[0]); "));
    BEZEL_EXPECT_EQ(named, std::string("3\n7"));
    BEZEL_EXPECT_EQ(positional, std::string("3\n7"));
}

// #3425's own repro, verbatim: stdlib §3.1 declares `constructor(initial:
// string)`, so `zzznotaparam` names no parameter. On `ef339fa` this compiled
// and printed "abc". `StringBuilder` declares TWO one-argument constructors
// (`capacity: int` and `initial: string`) that disagree on names, so the
// candidate-listing diagnostic fires and must name both spellings.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           rejects_nonsense_parameter_name_on_builtin_stringbuilder_constructor) {
    JitResult r = runAot(prog(
        "    var sb := new StringBuilder(zzznotaparam := \"ab\"); "
        "    sb.Append(\"c\"); "
        "    Console.WriteLine(sb.ToString()); "));
    if (!hasError(r, kNoCandidateDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kNoCandidateDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "(capacity) | (initial)"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// #3425 / stdlib §3.1: both of `StringBuilder`'s one-argument constructors
// must stay reachable by name — the overload-aware lookup (#3424) is what
// admits two candidates at one argument count, and a lookup that knew only one
// of them would reject the other.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           accepts_either_candidate_of_the_stringbuilder_constructor_pair) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    using var sb := new StringBuilder(initial := \"hi\"); "
                        "    Console.WriteLine(sb.ToString()); ")),
                    std::string("hi"));
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    using var sb := new StringBuilder(capacity := 128); "
                        "    Console.WriteLine(sb.Length); ")),
                    std::string("0"));
}

// ---------------------------------------------------------------------------
// The SHARED binder's four §22.5 violations, over a built-in CONSTRUCTOR.
// These pin "one binder, not a second copy": the strings are byte-identical to
// the ones a user-declared callee draws.
// ---------------------------------------------------------------------------

// §22.5: "Each parameter may be bound at most once" — positionally then by
// name here.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           rejects_parameter_bound_twice_on_builtin_constructor) {
    JitResult r = runAot(prog(
        "    var a := new Array<int>(3, size := 1); "
        "    Console.WriteLine(a.Count); "));
    if (!hasError(r, kBoundTwiceDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kBoundTwiceDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "`size`"));
}

// §22.5: "Named arguments may appear in any order AFTER positional arguments"
// — so a positional argument following a named one is an error.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           rejects_positional_after_named_on_builtin_constructor) {
    JitResult r = runAot(prog(
        "    var a := new Array<int>(size := 3, 7); "
        "    Console.WriteLine(a.Count); "));
    if (!hasError(r, kPositionalAfterNamedDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kPositionalAfterNamedDiag));
}

// §22.5: an unknown parameter name on a head with exactly ONE candidate at
// that argument count draws the shared binder's own wording.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           rejects_unknown_parameter_name_on_builtin_constructor) {
    JitResult r = runAot(prog(
        "    var a := new Array<int>(zzz := 3); "
        "    Console.WriteLine(a.Count); "));
    if (!hasError(r, kUnknownNameDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kUnknownNameDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "`zzz`"));
    BEZEL_EXPECT_TRUE(hasError(r, "constructor of built-in `Array`"));
}

// ---------------------------------------------------------------------------
// Limitation 1: a built-in constructor the registry carries no names for.
// ---------------------------------------------------------------------------

// #3425: stdlib §6.2 `Mutex` declares only `constructor()`, so the registry
// carries no one-argument names for it. The rejection must say WHY and what to
// do instead, rather than binding the name positionally behind the caller's
// back — the whole point of the fail-safe direction.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           rejects_named_arg_on_uncovered_builtin_constructor) {
    JitResult r = runAot(prog(
        "    using var m := new Mutex(zzz := 1); "));
    if (!hasError(r, kNoNamesDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kNoNamesDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Pass the arguments positionally"));
    BEZEL_EXPECT_TRUE(hasError(r, "#3425"));
}

// ---------------------------------------------------------------------------
// §22.5 OPTIONAL parameters on a built-in constructor (stdlib §2.5 `TimeOnly`
// declares `(hour, minute, second := 0, millisecond := 0)`).
// ---------------------------------------------------------------------------

// A trailing optional parameter left unsupplied is exactly what positional
// lowering already does, so the named form must be admitted.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           accepts_named_args_that_leave_trailing_optionals_unsupplied) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var t := new TimeOnly(hour := 1, minute := 2); "
                        "    Console.WriteLine(t.Hour * 100 + t.Minute); ")),
                    std::string("102"));
}

// …and SKIPPING an optional parameter while supplying a later one is a
// permutation the binder resolves (§22.5: a named argument binds the parameter
// it names; the skipped parameter keeps its declared default). Flipped by
// #7501 (narrow): it was rejected as "not supported" while codegen had no value
// for the skipped slot; the TimeOnly lowerNew arm now reads the published
// binding, so the skipped `second` is 0 and `millisecond` is 5.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           accepts_named_arg_that_skips_an_optional_parameter) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var t := new TimeOnly(1, 2, millisecond := 5); "
            "    Console.WriteLine($\"{t.Hour} {t.Minute} {t.Second} "
            "{t.Millisecond}\"); ")),
        std::string("1 2 0 5"));
}

// ---------------------------------------------------------------------------
// The built-in EXCEPTION constructors (spec §31.4).
// ---------------------------------------------------------------------------

// §31.4 declares `ArgumentException(message, paramName)`. In declaration order
// it constructs; reordered it is rejected rather than silently swapping the
// two strings — which is exactly the wrong-value shape on a two-`string`
// signature, where no type error can catch the swap.
// fix #3423 (§22.5, v0.283): reordered named args on exception constructors
// are now accepted and evaluate correctly (the binding is applied so the
// reordered call produces the same result as the in-order form).
BEZEL_TEST(builtin_ctor_named_arg_3425,
           binds_or_rejects_named_args_on_a_builtin_exception_constructor) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var e := new ArgumentException(message := \"bad\", "
            "paramName := \"p\"); "
            "    Console.WriteLine(e.Message); ")),
        std::string("bad"));
    // fix #3423: the reordered form now works correctly.
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    using var e := new ArgumentException(paramName := \"p\", "
            "message := \"bad\"); "
            "    Console.WriteLine(e.Message); ")),
        std::string("bad"));
}

// ---------------------------------------------------------------------------
// BRANCH-ORDER GUARDS — the two constructions that ALREADY bound by name must
// keep doing so. Adding an arm to a classify-and-report path can re-route
// other decisions; these rows are what would catch that.
// ---------------------------------------------------------------------------

// #3098 / §31.4: `ExceptionInfo`'s `lowerNew` arm resolves each argument to
// its FIELD SLOT by name and lowers in source order, so the reorder is
// honoured end to end and must stay ADMITTED
// (`common::isBuiltinCtorLoweredByName`). A fix that rejected every built-in
// reorder uniformly would break this — and it is not a hypothetical: the
// permuted spelling below is the one
// tests/codegen/builtin_record_positional_ctor_3098_test.cpp already pins.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           exceptioninfo_reordered_named_args_still_bind_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var e := new ExceptionInfo(Message := \"Msg\", "
            "StackTrace := \"Stack\", TypeName := \"TypeName\"); "
            "    Console.WriteLine(e.TypeName + \"|\" + e.Message + \"|\" + "
            "e.StackTrace); ")),
        std::string("TypeName|Msg|Stack"));
}

// §22.5 *Evaluation order*, on the ONE built-in construction this fix leaves
// admitting a reorder — which is exactly where an evaluation-order bug could
// hide, because every other reordered built-in construction is now rejected.
// §22.5 states it verbatim: "A call's **supplied** argument expressions are
// evaluated in **call-site textual order** — left to right as written, for
// positional and named arguments alike; naming an argument changes which
// parameter it binds to, never when it runs (`M(b := Next(), a := Next())`
// evaluates the `b` expression first)."
//
// The three arguments are side-effecting and distinguishable, so the two
// orders are separately observable: the VALUE line reports the BINDING
// (TypeName|Message|StackTrace = D, A, B) and the LOG line reports the
// EVALUATION order (A, B, D — the order written). A `lowerNew` arm that
// lowered in field order instead would print "DAB" and "DAB"; one that bound
// by position would print "ABD" and "ABD". Only "DAB" then "ABD" is §22.5.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           exceptioninfo_reordered_named_args_evaluate_in_textual_order) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "class C "
            "  public static method Tag(sb: StringBuilder, s: string): string "
            "    sb.Append(s); "
            "    return s; "
            "  end Tag; "
            "  public static method Main(): int "
            "    using var sb := new StringBuilder(); "
            "    var e := new ExceptionInfo(Message := Tag(sb, \"A\"), "
            "                               StackTrace := Tag(sb, \"B\"), "
            "                               TypeName := Tag(sb, \"D\")); "
            "    Console.WriteLine(e.TypeName + e.Message + e.StackTrace); "
            "    Console.WriteLine(sb.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("DAB\nABD"));
}

// …and an unknown name on that same exempt head is still rejected: the
// exemption is from the REORDER limitation only, never from §22.5's rule that
// a named argument names an existing parameter.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           exceptioninfo_unknown_parameter_name_is_still_rejected) {
    JitResult r = runAot(prog(
        "    var e := new ExceptionInfo(Message := \"Msg\", "
        "StackTrace := \"Stack\", zzz := \"TypeName\"); "
        "    Console.WriteLine(e.Message); "));
    if (!hasError(r, kUnknownNameDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kUnknownNameDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "`zzz`"));
}

// §22.5 / §17.7: a USER class's reordered named construction is legal and must
// still BIND BY NAME. The two field values are distinguishable, so a
// positional mis-bind is visible in the answer.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           user_declared_constructor_reordered_named_args_still_bind_by_name) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "class Pair "
            "  public a: int; "
            "  public b: int; "
            "  public constructor(a: int, b: int) "
            "    this.a := a; this.b := b; "
            "  end constructor; "
            "end Pair; "
            "class C "
            "  public static method Main(): int "
            "    using var p := new Pair(b := 20, a := 1); "
            "    Console.WriteLine(p.a * 100 + p.b); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("120"));
}

// #3425: an ALL-POSITIONAL built-in construction is untouched — the check
// claims only a `new` that actually writes a named argument.
BEZEL_TEST(builtin_ctor_named_arg_3425,
           accepts_positional_construction_on_uncovered_builtin_constructor) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    using var sb := new StringBuilder(); "
                        "    sb.Append(\"ab\"); "
                        "    Console.WriteLine(sb.ToString()); ")),
                    std::string("ab"));
}
