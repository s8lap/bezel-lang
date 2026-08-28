// Spec-derived regression test for issue #3398.
//
// spec §22.5 "Optional parameters and named arguments" (v0.283);
// stdlib §1 Conventions — *Window parameters* (v0.282/v0.283); §9.6
// `String`; §3.2 `Encoding`.
//
// WHAT THE DEFECT WAS. §22.5's argument binder (`ExprTC::bindArgsToParams`)
// ran for a user-declared callee (`checkCallArity`) and, since #3178, for an
// imported one (`checkImportedCallArity`). A BUILT-IN (stdlib,
// non-user-declared) member has no `MethodSymbol` at all, so NEITHER ran: the
// compiler carried no parameter names for any built-in member, the argument
// NAME was discarded outright, and every argument bound POSITIONALLY.
//
// MEASURED ON `e9bd762` / `d05399c` (build/bezelc, AOT, exit 0 on every line):
//
//   var s := "hello world";
//   s.Remove(startIndex := 5, length := 6)      -> "hello"   (correct)
//   s.Remove(nosuchparam := 5, alsonope := 6)   -> "hello"   (NONSENSE names
//                                                             accepted)
//   s.Remove(length := 6, startIndex := 5)      -> "hello "  <-- WRONG ANSWER:
//                                                  bound positionally as
//                                                  Remove(6, 5)
//
// The third row is the load-bearing one. §22.5 states "Named arguments may
// appear in any order after positional arguments", so the identical call on a
// user-declared method computes `Remove(5, 6)` and yields "hello". The
// built-in silently computed something else — a wrong-answer bug, not only a
// missing-rejection bug.
//
// WHAT THE FIX DOES, and its two deliberate limits. `common::builtinParamNames`
// now carries the declared parameter names for the stdlib §9.6 `string` family
// and the §3.2 `Encoding` family, and `ExprTC::checkBuiltinNamedArgs` feeds
// them to the SAME `bindArgsToParams` the user path uses — so an unknown name,
// a doubly-bound parameter, a positional argument after a named one, and a
// missing required parameter draw the user path's own §22.5 diagnostics. Two
// rejections are stated as implementation limitations rather than language
// rules:
//
//   1. A named argument on a built-in member the registry carries no names for
//      is REJECTED (rejecting an unknown name needs a COMPLETE list for that
//      member; a partial table must otherwise fall back to today's silent
//      positional binding).
//   2. A named argument that binds to a parameter OTHER than its own position
//      is REJECTED even where the names are known: codegen lowers a built-in
//      call by argument POSITION, and §22.5's *Evaluation order* rule forbids
//      buying the reorder by permuting the argument list ("A call's supplied
//      argument expressions are evaluated in call-site textual order").
//
// Both are LOUD. The project's standing preference is that a loud rejection
// beats a wrong value. Lifting limitation 2 (codegen consuming the binding)
// is #3423, still open.
//
// UPDATED BY fix #3424 (§22.5, v0.283). Limitation 1 is now bounded rather
// than absolute: `common::builtinParamNameCandidates` carries stdlib §3.1
// `StringBuilder` as well, and the lookup is OVERLOAD-AWARE — it returns every
// candidate name list declared at the call's argument count and the call is
// admitted if it binds against ANY of them. So the two rows below that pinned
// `sb.Append(zzz := …)` as "no names carried" and
// `s.Replace(oldValue := …, newValue := …)` as "same-arity overloads disagree,
// so reject" changed answers WITH the fix and are updated here in the same
// commit (the fix-must-update-existing-tests discipline): the first now draws
// the SHARED binder's unknown-name diagnostic, and the second COMPILES and
// yields §9.6's value. A member on a head the registry still carries no rows
// for (`Array<T>.IndexOf`) keeps limitation 1's rejection and is pinned below.
// The `new T(...)` twin is fix #3425
// (tests/sema/builtin_ctor_named_arg_binding_3425_test.cpp).
//
// FAIL-WITHOUT / PASS-WITH. Every `rejects_*` row below compiles clean on
// `d05399c` (the pre-fix tree) — the diagnostic text it asserts does not exist
// there at all — and every `accepts_*` / value row holds on both trees, so the
// accept rows are the false-rejection guard for the fix, not a restatement of
// it. The `user_declared_*` rows are the branch-order guard: adding an arm to
// a classify-and-report path can re-route OTHER diagnostics, so they assert
// the user-declared reorder still BINDS BY NAME and yields its value.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A whole program whose `Main` body is `body`. The observable is written to
// stdout, never to the exit status: a process exit status is masked to 8 bits
// (`WEXITSTATUS`), and these observables are strings.
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
// distinguish a genuine reject from a crash, and a bare-`hasErrors()` row
// would also silently survive a branch-order re-route that swapped one
// diagnostic for another.
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

// The #3398 limitation diagnostics, anchored on the issue number so a future
// §22.5 diagnostic cannot satisfy these rows by accident.
constexpr const char* kReorderDiag =
    "a named argument that binds to a parameter other than its own position "
    "is not supported on a built-in member";
constexpr const char* kNoNamesDiag =
    "named arguments are not supported on this built-in member";
// fix #3424: the candidate-listing diagnostic, reported when a built-in member
// has SEVERAL declared name lists at the call's argument count and the call
// binds against none of them (the shared binder can only name one list).
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
// The issue's repro — the WRONG-ANSWER row.
// ---------------------------------------------------------------------------

// #3398 / §22.5: the reordered named call on a built-in. On the pre-fix tree
// this compiled and printed "hello " (i.e. it evaluated `Remove(6, 5)`); the
// identical call on a user-declared `Remove(startIndex, length)` prints
// "hello". The value must never again be produced silently.
BEZEL_TEST(builtin_named_arg_3398,
           rejects_reordered_named_args_on_builtin_string_remove) {
    JitResult r = runAot(prog(
        "    var s := \"hello world\"; "
        "    Console.WriteLine(s.Remove(length := 6, startIndex := 5)); "));
    if (!hasError(r, kReorderDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kReorderDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "#3398"));
    // The diagnostic must name the offending parameter and both positions, so
    // the reader can see WHICH argument moved. The loop reports the FIRST
    // formal whose binding is not the identity, i.e. parameter 1.
    BEZEL_EXPECT_TRUE(hasError(r, "`startIndex` is parameter 1 but is written "
                                  "as argument 2"));
}

// #3398 / §22.5: nonsense parameter names on a built-in were accepted and
// bound positionally. §22.5 says a named argument names an EXISTING parameter;
// the rejection is the SHARED binder's, so it carries the same wording a
// user-declared callee's unknown name draws.
BEZEL_TEST(builtin_named_arg_3398,
           rejects_unknown_parameter_name_on_builtin_string_remove) {
    JitResult r = runAot(prog(
        "    var s := \"hello world\"; "
        "    Console.WriteLine(s.Remove(nosuchparam := 5, alsonope := 6)); "));
    if (!hasError(r, kUnknownNameDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kUnknownNameDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "`nosuchparam`"));
    BEZEL_EXPECT_TRUE(hasError(r, "\xC2\xA7""22.5"));
}

// #3398 / stdlib §1 (v0.282 window-parameter renames): the OLD parameter name
// `count` was renamed to `length` on `String.Remove`. Before this fix the
// rename was a no-op in the compiler — no name existed either side of it — so
// the stale spelling compiled. It must now be rejected as the unknown name it
// is, which is what makes a stdlib parameter rename a real API-surface change.
BEZEL_TEST(builtin_named_arg_3398,
           rejects_pre_v0282_window_parameter_spelling) {
    JitResult r = runAot(prog(
        "    var s := \"hello world\"; "
        "    Console.WriteLine(s.Remove(startIndex := 5, count := 6)); "));
    if (!hasError(r, kUnknownNameDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kUnknownNameDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "`count`"));
}

// ---------------------------------------------------------------------------
// The SHARED binder's other three §22.5 violations, over a built-in callee.
// These pin "one binder, not a second copy": the strings are byte-identical to
// the ones a user-declared callee draws.
// ---------------------------------------------------------------------------

// §22.5: "Each parameter may be bound at most once" — positionally then by
// name here.
BEZEL_TEST(builtin_named_arg_3398,
           rejects_parameter_bound_twice_on_builtin) {
    JitResult r = runAot(prog(
        "    var s := \"hello world\"; "
        "    Console.WriteLine(s.Remove(5, startIndex := 1)); "));
    if (!hasError(r, kBoundTwiceDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kBoundTwiceDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "`startIndex`"));
}

// §22.5: "Named arguments may appear in any order AFTER positional arguments"
// — so a positional argument following a named one is an error.
BEZEL_TEST(builtin_named_arg_3398,
           rejects_positional_after_named_on_builtin) {
    JitResult r = runAot(prog(
        "    var s := \"hello world\"; "
        "    Console.WriteLine(s.Remove(startIndex := 5, 6)); "));
    if (!hasError(r, kPositionalAfterNamedDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kPositionalAfterNamedDiag));
}

// ---------------------------------------------------------------------------
// Limitation 1: a built-in member the registry carries no names for.
// ---------------------------------------------------------------------------

// #3398 limitation 1, on a head the registry STILL carries no member rows for.
// `Array<T>` is a reserved built-in name, so the check claims the call — and
// with no names to match against, the name is rejected with the limitation
// named rather than bound positionally behind the caller's back. The
// diagnostic must say WHY and what to do instead.
//
// (This row was `sb.Append(zzz := …)` before fix #3424 widened the table to
// stdlib §3.1; `StringBuilder` now HAS names, so that spelling draws the
// shared binder's unknown-name diagnostic instead — pinned separately below.)
BEZEL_TEST(builtin_named_arg_3398,
           rejects_named_arg_on_uncovered_builtin_member) {
    JitResult r = runAot(prog(
        "    var a: Array<int> := [1, 2, 3]; "
        "    Console.WriteLine(a.IndexOf(item := 2)); "));
    if (!hasError(r, kNoNamesDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kNoNamesDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "`Array.IndexOf`"));
    BEZEL_EXPECT_TRUE(hasError(r, "Pass the arguments positionally"));
    BEZEL_EXPECT_TRUE(hasError(r, "#3398"));
}

// fix #3424 / stdlib §3.1: `StringBuilder` is covered now, so an unknown name
// on it draws the SHARED binder's diagnostic — the one a user-declared callee
// draws — rather than limitation 1's "no names carried" message. This is the
// row that proves the widening turned a not-supported rejection into a real
// §22.5 name check.
BEZEL_TEST(builtin_named_arg_3398,
           unknown_name_on_newly_covered_stringbuilder_draws_the_shared_diagnostic) {
    JitResult r = runAot(prog(
        "    using var sb := new StringBuilder(); "
        "    sb.AppendLine(zzz := \"x\"); "));
    if (!hasError(r, kUnknownNameDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kUnknownNameDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "`StringBuilder.AppendLine`"));
    BEZEL_EXPECT_TRUE(hasError(r, "`zzz`"));
    BEZEL_EXPECT_FALSE(hasError(r, kNoNamesDiag));
}

// fix #3424 / stdlib §3.1: and the in-order named call on the newly covered
// head COMPILES and computes the declared value. Rejected before the fix
// (limitation 1), so this row is FAIL-WITHOUT for the widening.
BEZEL_TEST(builtin_named_arg_3398,
           accepts_in_order_named_args_on_stringbuilder_append) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    using var sb := new StringBuilder(); "
                        "    sb.Append(value := \"ab\"); "
                        "    Console.WriteLine(sb.ToString()); ")),
                    std::string("ab"));
}

// fix #3424 / stdlib §9.6: `Replace/2` was deliberately OMITTED from #3398's
// table — §9.6 declares two arity-2 overloads that DISAGREE on parameter names
// (`oldChar, newChar` and `oldValue, newValue`) and the single-list lookup
// could not decide between them, so a call naming EITHER pair was rejected.
// The overload-aware lookup returns both candidate lists and admits a call
// that binds against either, so this now compiles and computes §9.6's value.
// (Was `rejects_named_arg_on_same_arity_name_disagreeing_overload`.)
BEZEL_TEST(builtin_named_arg_3398,
           accepts_named_args_on_same_arity_name_disagreeing_overload) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"hello\"; "
            "    Console.WriteLine(s.Replace(oldValue := \"l\", "
            "newValue := \"L\")); ")),
        std::string("heLLo"));
}

// fix #3424: …and the OTHER candidate of the same overload pair, whose
// arguments are `char`s. Both candidates must be reachable, or the lookup has
// merely swapped which single overload it knows about.
BEZEL_TEST(builtin_named_arg_3398,
           accepts_the_other_candidate_of_a_name_disagreeing_overload_pair) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"hello\"; "
            "    Console.WriteLine(s.Replace(oldChar := \'l\', "
            "newChar := \'L\')); ")),
        std::string("heLLo"));
}

// fix #3424: names taken from DIFFERENT candidates of the same overload pair
// belong to no declared overload and must still be rejected — the candidate
// rule is "binds against one list", never "every name appears in some list".
// The diagnostic lists both declared spellings, because the shared binder can
// only name one of them.
BEZEL_TEST(builtin_named_arg_3398,
           rejects_names_mixed_across_two_candidates_of_one_overload_pair) {
    JitResult r = runAot(prog(
        "    var s := \"hello\"; "
        "    Console.WriteLine(s.Replace(oldChar := \'l\', "
        "newValue := \"L\")); "));
    if (!hasError(r, kNoCandidateDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kNoCandidateDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "(oldChar, newChar) | (oldValue, newValue)"));
}

// ---------------------------------------------------------------------------
// ACCEPT rows — the false-rejection guard. Each of these holds on the pre-fix
// tree too; the fix must not change what they compute.
// ---------------------------------------------------------------------------

// §22.5 / stdlib §9.6: an IN-ORDER named call on a covered built-in still
// compiles AND still evaluates to the §9.6-declared result.
BEZEL_TEST(builtin_named_arg_3398,
           accepts_in_order_named_args_on_builtin_string_remove) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var s := \"hello world\"; "
            "    Console.WriteLine(s.Remove(startIndex := 5, length := 6)); ")),
        std::string("hello"));
}

// §22.5: a single named argument on a covered built-in, and the ordinary
// all-positional form — neither may change.
BEZEL_TEST(builtin_named_arg_3398,
           accepts_single_named_and_positional_forms_on_builtin) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var s := \"hello world\"; "
                        "    Console.WriteLine(s.Substring(startIndex := 6)); ")),
                    std::string("world"));
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var s := \"hello world\"; "
                        "    Console.WriteLine(s.Remove(5, 6)); ")),
                    std::string("hello"));
}

// §22.5: a mixed positional-then-named call binds exactly as the all-
// positional one does. This is the row that proves the accept path runs the
// binder rather than merely skipping the check when a name is present.
BEZEL_TEST(builtin_named_arg_3398,
           accepts_positional_then_named_on_builtin) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var s := \"hello world\"; "
                        "    Console.WriteLine(s.Remove(5, length := 6)); ")),
                    std::string("hello"));
}

// #3398: an ALL-POSITIONAL built-in call on an UNCOVERED member is untouched —
// the check claims only calls that actually write a named argument.
BEZEL_TEST(builtin_named_arg_3398,
           accepts_positional_call_on_uncovered_builtin_member) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var sb := new StringBuilder(); "
                        "    sb.Append(\"ab\"); "
                        "    Console.WriteLine(sb.ToString()); ")),
                    std::string("ab"));
}

// ---------------------------------------------------------------------------
// The other two receiver SHAPES the check must reach. Each is a distinct
// AST/TypeRef shape, not a restatement of the `string` instance rows: a
// receiver whose head arrives only through a `builtinClassSym` declaration,
// and the STATIC spelling whose target is a bare identifier with no value
// type. Both were measured accepting nonsense names before the fix.
// ---------------------------------------------------------------------------

// #3398 / stdlib §3.2: `Encoding.UTF8` is typed with
// `declaration = builtinClassSym("Encoding")` — a NON-null declaration — so it
// reaches the check through the intrinsic-TypeSymbol arm rather than the
// declaration-less one. In-order named arguments must still compute the
// §3.2-declared window.
BEZEL_TEST(builtin_named_arg_3398,
           accepts_in_order_named_args_on_encoding_getstring) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var by := Encoding.UTF8.GetBytes(\"hello\"); "
            "    Console.WriteLine(Encoding.UTF8.GetString(bytes := by, "
            "start := 1, length := 3)); ")),
        std::string("ell"));
}

// #3398 / stdlib §3.2 / §1: the reordered form. On the pre-fix tree this did
// not merely mis-evaluate — the arguments were passed in WRITTEN order, so
// codegen emitted malformed IR ("SExt only operates on integer", "Call
// parameter type does not match function signature"). §22.5's rejection
// replaces a codegen ICE with a diagnostic that names the problem.
BEZEL_TEST(builtin_named_arg_3398,
           rejects_reordered_named_args_on_encoding_getstring) {
    JitResult r = runAot(prog(
        "    var by := Encoding.UTF8.GetBytes(\"hello\"); "
        "    Console.WriteLine(Encoding.UTF8.GetString(length := 3, "
        "start := 1, bytes := by)); "));
    if (!hasError(r, kReorderDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kReorderDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "`Encoding.GetString`"));
}

// #3398 / stdlib §3.2: the issue's own `Encoding` row — nonsense names were
// accepted and the call ran, printing "ell".
BEZEL_TEST(builtin_named_arg_3398,
           rejects_unknown_parameter_name_on_encoding_getstring) {
    JitResult r = runAot(prog(
        "    var by := Encoding.UTF8.GetBytes(\"hello\"); "
        "    Console.WriteLine(Encoding.UTF8.GetString(aaa := by, bbb := 1, "
        "ccc := 3)); "));
    if (!hasError(r, kUnknownNameDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kUnknownNameDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "`aaa`"));
}

// #3398 / stdlib §9.6 / §1: the STATIC spelling. `String.From(buffer, start,
// length)` is one of the three signatures stdlib §1 renamed at v0.282, and
// its target is a bare identifier carrying no value type — the third receiver
// shape. In-order named arguments must compute the declared window.
BEZEL_TEST(builtin_named_arg_3398,
           accepts_in_order_named_args_on_static_string_from) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var buf: Array<char> := ['a', 'b', 'c']; "
            "    Console.WriteLine(String.From(buffer := buf, start := 1, "
            "length := 2)); ")),
        std::string("bc"));
}

// #3398 / stdlib §9.6: the issue's `String.From(zzz := buf, qqq := 1,
// www := 2)` row — accepted on the pre-fix tree, printing "bc".
BEZEL_TEST(builtin_named_arg_3398,
           rejects_unknown_parameter_name_on_static_string_from) {
    JitResult r = runAot(prog(
        "    var buf: Array<char> := ['a', 'b', 'c']; "
        "    Console.WriteLine(String.From(zzz := buf, qqq := 1, www := 2)); "));
    if (!hasError(r, kUnknownNameDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kUnknownNameDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "`String.From`"));
}

// #3398: the static reorder.
BEZEL_TEST(builtin_named_arg_3398,
           rejects_reordered_named_args_on_static_string_from) {
    JitResult r = runAot(prog(
        "    var buf: Array<char> := ['a', 'b', 'c']; "
        "    Console.WriteLine(String.From(length := 2, start := 1, "
        "buffer := buf)); "));
    if (!hasError(r, kReorderDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kReorderDiag));
}

// ---------------------------------------------------------------------------
// BRANCH-ORDER GUARD — the user-declared path must be untouched. §22.5's
// reorder is legal there and must still BIND BY NAME (not positionally).
// ---------------------------------------------------------------------------

BEZEL_TEST(builtin_named_arg_3398,
           user_declared_reordered_named_args_still_bind_by_name) {
    // `Cut(startIndex, length)` mirrors §9.6's `Remove`; the reordered named
    // call must compute Cut(5, 6) — "hello" — not Cut(6, 5).
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "class C "
            "  public static method Cut(startIndex: int, length: int): string "
            "    var s := \"hello world\"; "
            "    return s.Remove(startIndex, length); "
            "  end Cut; "
            "  public static method Main(): int "
            "    Console.WriteLine(C.Cut(length := 6, startIndex := 5)); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        std::string("hello"));
}

BEZEL_TEST(builtin_named_arg_3398,
           user_declared_unknown_name_still_draws_the_shared_diagnostic) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method One(x: int): int "
        "    return x; "
        "  end One; "
        "  public static method Main(): int "
        "    Console.WriteLine(C.One(nosuch := 1)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, kUnknownNameDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kUnknownNameDiag));
    // …and it must NOT be re-routed to either #3398 limitation message.
    BEZEL_EXPECT_FALSE(hasError(r, kNoNamesDiag));
    BEZEL_EXPECT_FALSE(hasError(r, kReorderDiag));
}
