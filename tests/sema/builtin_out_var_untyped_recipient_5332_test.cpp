// #5332 — `OwningSet<T>.TryPop(query, out using var b)` left the inline
// recipient UNTYPED, so every read of it failed to lower.
//
// WHAT THE DOCUMENTS REQUIRE.  Stdlib §4.10 (v0.313) declares
// `public method TryPop(query: T, out owned removed: T?): bool;` on
// `OwningSet<T>`, and stdlib §1 shape (a) puts `OwningSet<T>.TryPop` in the
// "bool + nullable out" table ("keyed/positional extraction: …
// `OwningSet<T>.TryPop` …").  Spec §22.4 (v0.313): "`out var name` declares a
// new variable inline. Its type is inferred from the method's `out` parameter
// declaration" — and the `out using var` form is the same declaration (spec
// §22.4 / §28.13).  So the inline recipient of `TryPop` is a `R?` local, and a
// read of it on the `when true` path (spec §22.4, #5312's shape-(a) narrowing)
// is a member access on a non-null `R`.
//
// THE DEFECT.  The BUILTIN borrowing-extraction derivation for a `Try*` callee
// with no `MethodDecl` (src/sema/ExprTypecheck_Exprs.cpp, "BUILTIN borrowing
// extraction Trys have no MethodDecl", #1336) carries an arm per builtin
// collection.  It had Stack / Queue / OwningStack / OwningQueue / Channel /
// Dictionary / OwningDictionary arms but NO `OwningSet` arm, so the recipient's
// `declType` stayed null.  The `out using var` arm chain had the same hole
// (its `OwningSet` entry was for the pre-rename `TryTake` only).
//
// MEASURED PRE-FIX (staging r23-c28 d27e5597+e53369d3, `agent_check --probe
// --asan`): the issue's repro emits
//   `internal: the argument of this Console.WriteLine failed to lower to a
//    value, so the whole statement emits nothing — not even the line
//    terminator … #3079`
// and prints nothing.  The plain `out var` twin of the same call emits the
// same warning.
//
// SIBLING AUDIT (the issue asked for it; each probed on the same base).
// Measured by `agent_check --probe --asan`, one-element collection, reading
// `.X` / `.Val` off the inline `out using var` recipient:
//   OwningArray<T>.TryPopFirst       prints 10 — ALREADY CORRECT
//   OwningArray<T>.TryPopLast        prints 20 — ALREADY CORRECT
//   OwningLinkedList<T>.TryPopFirst  prints  7 — ALREADY CORRECT
//   OwningLinkedList<T>.TryPopLast   prints  8 — ALREADY CORRECT
//   OwningDictionary<K,V>.TryPopValue prints 11 — ALREADY CORRECT
// Each is pinned below as a POSITIVE CONTROL (green before and after this
// fix): the derivation table already carries their arms, so the issue body's
// "print correctly only because something else types them" is resolved — it is
// the table arm, not an accident.
//
// NOT FIXED HERE (codegen, a different slot — filed as a follow-up).
// `OwningSet<T>.Pop` and `OwningSet<T>.TryPop` had NO codegen handler when this
// file was written: LlvmBackend_Calls.cpp lowered the PRE-RENAME
// `OwningSet<T>.Take` / `.TryTake` (stdlib §4.10 renamed the pair to `Pop` /
// `TryPop`), and nothing dispatched the new names.  MEASURED at the time with
// a pre-declared recipient — `var b: R? := null; Console.WriteLine(oset.TryPop
// (new R(9), out b));` — which bypasses this lane's arm entirely and still
// reported `internal: the argument of this Console.WriteLine failed to lower to
// a value`.  So the issue's stated post-fix expectation (stdout `9`) was NOT
// reachable from a sema-only change, and the `OwningSet` AOT rows below pinned
// what the CALL-SITE typing fix actually delivered.
//
// THAT FOLLOW-UP HAS LANDED: `7dff597d fix #5361: add OwningSet<T>.Pop /
// TryPop codegen lowering (stdlib §4.10, v0.259)` is on r23-c28.  MEASURED
// again on this base with `scripts/agent_check.sh --probe --asan`: the issue's
// repro now prints `9` and exits 0, and the `internal: … failed to lower`
// warning is gone.  The two `owningset_trypop_*_200x_no_leak` rows keep their
// narrower claim (they pin the loop's own allocations being released, NOT that
// the set was drained), and the issue-repro row's `want` has been corrected to
// the measured `9` — see the row for why the old `""` was an inverted pin.

#include <string>

#include "support/aot_support.hpp"
#include "support/da_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// ---------------------------------------------------------------------------
// Sema-side oracle.
//
// `runAot` fills `diagnostics` only on a REJECT (aot_support.hpp), so the
// warning a successful compile emits is invisible to it.  The defect here is a
// TYPING defect observable without codegen at all: an UNTYPED local accepts
// every assignment, so `var s: R := b11;` (a `R?` → `R` narrowing, which the
// spec forbids) is ACCEPTED before the fix and REJECTED after it.  That is the
// pin; it distinguishes "typed `R?`" from both "untyped" and "typed `R`".
// ---------------------------------------------------------------------------

// A record type that satisfies OwningSet<T>'s `where T: class` (stdlib §4.2)
// and can be the set's hash/equality witness.
constexpr const char* kElemDecl =
    "class R "
    "  public X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  destructor end destructor; "
    "  public override method Equals(other: Object?): bool "
    "    if other is R then var b := other as R; return b!.X = X; end; "
    "    return false; end Equals; "
    "  public override method GetHashCode(): int => X; "
    "end R; ";

// `body` is the Main body; `RECIP_DECL` is the inline out-recipient form under
// test.  The program is DA-checked, so an untyped recipient shows up as an
// accepted (not rejected) source.
std::string setProgram(const std::string& recipDecl,
                       const std::string& body) {
    return std::string("module M; ") + kElemDecl +
           "class C public static method Main(): int "
           "  using var oset := new OwningSet<R>(); "
           "  oset.Add(move (new R(9))); " +
           body + " return 0; end Main; end C;";
}

bool rejected(const std::string& src) {
    return bezel::test::parseAndCheckDA(src).diagnostics.hasErrors();
}

// ---- the pin: `out using var` recipient of OwningSet<R>.TryPop -------------

// POST-FIX (rejected): the recipient is `R?`, and stdlib §1 shape (a) says the
// false path assigns `null`, so it can never be assigned to a `R` without a
// null check.  PRE-FIX (accepted): the recipient is UNTYPED and every
// assignment is waved through — the issue's defect, observed without codegen.
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningset_trypop_using_recipient_typed_nullable_22_4) {
    BEZEL_EXPECT_TRUE(rejected(setProgram(
        "out using var b11",
        "  oset.TryPop(new R(9), out using var b11); "
        "  var s: R := b11; ")));
}

// The same source with the widening assignment is ACCEPTED — the recipient is
// `R?`, not an error type and not `R`.  Guards the arm against over-typing
// (a non-nullable `R` would reject this).
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningset_trypop_using_recipient_widens_to_nullable_22_4) {
    BEZEL_EXPECT_FALSE(rejected(setProgram(
        "out using var b11",
        "  oset.TryPop(new R(9), out using var b11); "
        "  var s: R? := b11; ")));
}

// The plain `out var` twin.  The `out var` derivation is a SEPARATE table from
// the `out using var` one and had the same `OwningSet` hole; spec §22.4 gives
// both forms the same "inferred from the method's `out` parameter declaration"
// rule.
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningset_trypop_out_var_recipient_typed_nullable_22_4) {
    BEZEL_EXPECT_TRUE(rejected(setProgram(
        "out var b11",
        "  oset.TryPop(new R(9), out var b11); "
        "  var s: R := b11; ")));
}

// A control that would be vacuous as a pin (an UNTYPED local also accepts
// `var s: R? := …`) but is NOT vacuous against over-typing: the `out var`
// recipient widens cleanly to `R?`.
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningset_trypop_out_var_recipient_widens_to_nullable_22_4) {
    BEZEL_EXPECT_FALSE(rejected(setProgram(
        "out var b11",
        "  oset.TryPop(new R(9), out var b11); "
        "  var s: R? := b11; ")));
}

// An unrelated member access on the recipient is still rejected — the arm
// types the recipient `R?`, it does not launder the program.
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningset_trypop_using_recipient_still_nullable_22_4) {
    BEZEL_EXPECT_TRUE(rejected(setProgram(
        "out using var b11",
        "  oset.TryPop(new R(9), out using var b11); "
        "  var s: string := b11; ")));
}

// ---------------------------------------------------------------------------
// Sibling positive controls — the audit rows.
//
// Each names a builtin `Try*` whose derivation arm ALREADY existed on the base
// (measured: it printed correctly before this fix), so the row is green
// pre-fix and post-fix.  The sema controls assert the arm's `T?` typing; the
// AOT controls assert the read lowers AND prints the element.
// ---------------------------------------------------------------------------

constexpr const char* kBoxDecl =
    "module M; class Box "
    "  public Val: int := 0; "
    "  public constructor(v: int) Val := v; end constructor; "
    "  destructor end destructor; "
    "end Box; ";

std::string siblingProgram(const std::string& recvDecl, const std::string& call,
                           const std::string& after) {
    return std::string(kBoxDecl) + "class C public static method Main(): int " +
           recvDecl + call + after + " return 0; end Main; end C;";
}

// ---- OwningArray<T>.TryPopFirst / TryPopLast --------------------------------

BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningarray_trypopfirst_recipient_typed_nullable_control_22_4) {
    BEZEL_EXPECT_TRUE(rejected(siblingProgram(
        "using var a := new OwningArray<Box>(); a.Add(move (new Box(10)));",
        "a.TryPopFirst(out using var b);", "var s: Box := b;")));
}

BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningarray_trypoplast_recipient_typed_nullable_control_22_4) {
    BEZEL_EXPECT_TRUE(rejected(siblingProgram(
        "using var a := new OwningArray<Box>(); a.Add(move (new Box(10)));",
        "a.TryPopLast(out using var b);", "var s: Box := b;")));
}

// ---- OwningLinkedList<T>.TryPopFirst / TryPopLast --------------------------

BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owninglinkedlist_trypopfirst_recipient_typed_nullable_control_22_4) {
    BEZEL_EXPECT_TRUE(rejected(siblingProgram(
        "using var l := new OwningLinkedList<Box>(); l.AddLast(move (new Box(7)));",
        "l.TryPopFirst(out using var b);", "var s: Box := b;")));
}

BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owninglinkedlist_trypoplast_recipient_typed_nullable_control_22_4) {
    BEZEL_EXPECT_TRUE(rejected(siblingProgram(
        "using var l := new OwningLinkedList<Box>(); l.AddLast(move (new Box(7)));",
        "l.TryPopLast(out using var b);", "var s: Box := b;")));
}

// ---- OwningDictionary<K,V>.TryPopValue ------------------------------------

BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningdictionary_trypopvalue_recipient_typed_nullable_control_22_4) {
    BEZEL_EXPECT_TRUE(rejected(siblingProgram(
        "using var d := new OwningDictionary<string, Box>(); "
        "d.Add(\"k\", move (new Box(11)));",
        "d.TryPopValue(\"k\", out using var b);", "var s: Box := b;")));
}

// ---------------------------------------------------------------------------
// AOT controls and leak rows.
//
// These run the real driver end to end.  They are the audit's "does it print
// today" answer, and they keep the 200-iteration shape the leak rows need.
// ---------------------------------------------------------------------------

void expect(const std::string& src, const std::string& want) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// OwningArray.TryPopFirst: ALREADY CORRECT pre-fix (measured `10`).  Pins the
// read as lowering to the element's value, not a degraded default.
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningarray_trypopfirst_already_correct_control) {
    expect(siblingProgram(
               "using var a := new OwningArray<Box>(); a.Add(move (new Box(10)));",
               "if a.TryPopFirst(out using var b) then Console.WriteLine(b.Val); end;",
               ""),
           "10\n");
}

// OwningArray.TryPopLast: ALREADY CORRECT pre-fix (measured `20`).
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningarray_trypoplast_already_correct_control) {
    expect(siblingProgram(
               "using var a := new OwningArray<Box>(); a.Add(move (new Box(10))); a.Add(move (new Box(20)));",
               "if a.TryPopLast(out using var b) then Console.WriteLine(b.Val); end;",
               ""),
           "20\n");
}

// OwningLinkedList.TryPopFirst: ALREADY CORRECT pre-fix (measured `7`).
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owninglinkedlist_trypopfirst_already_correct_control) {
    expect(siblingProgram(
               "using var l := new OwningLinkedList<Box>(); l.AddLast(move (new Box(7)));",
               "if l.TryPopFirst(out using var b) then Console.WriteLine(b.Val); end;",
               ""),
           "7\n");
}

// OwningLinkedList.TryPopLast: ALREADY CORRECT pre-fix (measured `8`).
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owninglinkedlist_trypoplast_already_correct_control) {
    expect(siblingProgram(
               "using var l := new OwningLinkedList<Box>(); l.AddLast(move (new Box(7))); l.AddLast(move (new Box(8)));",
               "if l.TryPopLast(out using var b) then Console.WriteLine(b.Val); end;",
               ""),
           "8\n");
}

// OwningDictionary.TryPopValue: ALREADY CORRECT pre-fix (measured `11`).
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningdictionary_trypopvalue_already_correct_control) {
    expect(siblingProgram(
               "using var d := new OwningDictionary<string, Box>(); d.Add(\"k\", move (new Box(11)));",
               "if d.TryPopValue(\"k\", out using var b) then Console.WriteLine(b.Val); end;",
               ""),
           "11\n");
}

// A GENUINE 200-iteration drain exercise on a path that does lower: each
// iteration transfers one element out of the OwningArray into the typed
// `out using var` recipient, which is disposed at scope end.  A per-iteration
// leak on either the transfer or the disposal shows up as a LeakSanitizer
// report on this row.
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningarray_trypopfirst_200x_no_leak) {
    expect(siblingProgram(
               "using var a := new OwningArray<Box>(); var i := 0;",
               "while i < 200 do a.Add(move (new Box(i))); i := i + 1; end; "
               "var n := 0; "
               "while a.TryPopFirst(out using var b) do n := n + 1; end; "
               "Console.WriteLine(n);",
               ""),
           "200\n");
}

// The OwningSet path, 200 iterations with a FRESH-OWNED query
// (`new R(9)` handed to a `query: T` borrow-taking parameter, stdlib §4.10).
// The call site no longer emits the `internal: … failed to lower` warning, the
// loop runs to completion, and nothing leaks.  This row deliberately does NOT
// claim the set was drained: `OwningSet<T>.TryPop` has no codegen handler
// (see the header), so what is pinned here is the call-site typing fix's own
// surface — the loop's own allocations are all released.
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningset_trypop_200x_no_leak) {
    expect(setProgram("out using var b11",
                      "  var i := 0; "
                      "  while i < 200 do "
                      "    using var oset2 := new OwningSet<R>(); "
                      "    oset2.Add(move (new R(9))); "
                      "    oset2.TryPop(new R(9), out using var b2); "
                      "    i := i + 1; "
                      "  end; "
                      "  Console.WriteLine(\"done\"); "),
           "done\n");
}

// The same loop with a PRE-DECLARED, explicitly disposed query, so the row
// isolates the recipient (not the query's ownership) as the thing under test.
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningset_trypop_prequery_200x_no_leak) {
    expect(setProgram("out using var b11",
                      "  var i := 0; "
                      "  while i < 200 do "
                      "    using var oset2 := new OwningSet<R>(); "
                      "    oset2.Add(move (new R(9))); "
                      "    var q: R := new R(9); "
                      "    oset2.TryPop(q, out using var b2); "
                      "    q.Dispose(); "
                      "    i := i + 1; "
                      "  end; "
                      "  Console.WriteLine(\"done\"); "),
           "done\n");
}

// The issue's own repro, verbatim.  It compiles and runs clean (the
// `internal: … failed to lower` warning is gone, exit 0) AND prints the
// element's `X` — the issue's stated expectation.
//
// The `want` was MEASURED, not reasoned: `scripts/agent_check.sh --probe
// --asan` on this exact program prints `9` (and runs the #5361 codegen
// handler added on r23-c28, `7dff597d fix #5361: add OwningSet<T>.Pop /
// TryPop codegen lowering`).  The previous `want` was `""` — the value the
// PRE-#5361 state produced — so the row would have passed before that fix
// and failed after it: an inverted pin, vacuous with respect to the #5332
// call-site typing fix it is named for.
//
// NON-VACUOUS in both directions, which the empty-string pin was not:
//   * If the #5332 arm regresses, the recipient is UNTYPED again, the
//     `Console.WriteLine(b11.X)` argument fails to lower and the statement
//     emits nothing (the pre-#5332 symptom this file's header records), so
//     stdout is `""` — the row FAILS.
//   * If the #5361 lowering regresses the same way, stdout is `""` again —
//     the row FAILS.
// Asserting the element's own value (not merely "ran") is what makes it
// bite: an empty-string expectation is satisfied by the very breakage it is
// supposed to detect.
BEZEL_TEST(builtin_out_var_untyped_recipient_5332,
           owningset_trypop_issue_repro_runs_clean) {
    expect(setProgram("out using var b11",
                      "  if oset.TryPop(new R(9), out using var b11) "
                      "    then Console.WriteLine(b11.X); end; "),
           "9\n");
}

}  // namespace
