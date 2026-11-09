// Codegen regression tests for issues #7861 and #7858: §17.13 *The discard
// `_`* (the owning-CHOICE production arm), §35.4 *The classification* (the
// owning choice's active-case disposal), and §17.8 *The null-forgiving
// operator `!`* (a `!` read is the bare read) — spec v0.355.
//
// #7861 — `[leak] _ := Ret() where Ret returns an owning choice disposes only
// half the payloads (100 of 200)`.
//
// §17.13: "**An owned result is disposed at the end of the discard statement.**
// If the result is an owned value — a class instance, an interface-typed
// value, or a move-only value (§15.3, §35.4) — it is an *unbound owned
// temporary* (§28.3): `_ := expression;` evaluates `expression` and discharges
// the disposal obligation once the statement completes. ... For a **move-only
// value** (an owning inline array `[N owned]T?`/`[N owned]T`, or an
// owning-classified choice) its owned slots — for a choice, the active case's
// owned payloads — are disposed in the usual order."
//
// The CLASS half of that rule was implemented (#1009's `disposeOwnedValue`),
// the owning-INLINE-ARRAY half (#1016's `disposeInlineOwnedArray`) and the
// owning-COLLECTION half (#1909's `disposeOwnedCollectionValue`) — but the
// owning-CHOICE half had NO arm, so the discarded choice was owned by nothing
// and its active case's payloads leaked outright. MEASURED at r30-c1
// (7d4c6614): `_ := Ret();` printed `100` where `200` is the correct count
// (two `Resource`s per iteration — the caller's own local and the callee's)
// and LeakSanitizer reported "1600 byte(s) leaked in 100 allocation(s)".
//
// #7858 — `[codegen][UAF] field store h.G := o! of an owning-choice local
// into an owned field disposes the payload twice`.
//
// §17.8: "Over a named owned binding, `x!` is therefore classified by its
// position exactly as the bare read `x` is ... `sink._f := x!;` consumes `x`
// exactly as `sink._f := x;` does" (spec:3662). The field-store caller of
// `markOwningChoiceMovedFromCore` did NOT peel the `!`, and that core's source
// test is a BARE IDENTIFIER — so the mark was declined, the taker kept its
// scope-end active-case disposal, and the payload the field had already taken
// was disposed a second time. MEASURED at r30-c1 (7d4c6614): AddressSanitizer
// heap-use-after-free. The differential control is the PLAIN spelling
// (`h.G := o`), which was clean and stays clean.
//
// THE ORACLE. Each row pins the exact DISPOSAL COUNT, not "did not crash": a
// counting destructor gives one increment per disposal, so a doubled disposal
// (a double free) and a suppressed one (a leak) are distinguishable from each
// other and from the spec's answer. `ran` and `exitCode` are asserted
// alongside the text because `runAot` reports a signal death as
// `ran == false` / `exitCode == -1`, which is exactly the pre-fix observable
// for the #7858 rows.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A COUNTING destructor, so a row can pin the exact number of disposals. A
// one-shot double free can pass on both sides of a fix (the second dispatch may
// land in still-mapped memory), so the strong oracle is the count.
const char* kCountingDecls =
    "class Resource "
    "  public static Count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Resource.Count := Resource.Count + 1; end destructor; "
    "end Resource; "
    "choice Bundle Ready(owned r: Resource), Empty; end Bundle; "
    "class Holder "
    "  public owned G: Bundle? := null; "
    "  public constructor() end constructor; "
    "end Holder; ";

bezel::test::JitResult run(const std::string& body,
                           const std::string& moduleDecls = "",
                           const std::string& members = "") {
    return runAot(std::string("module M; ") + kCountingDecls + moduleDecls +
                  " class Host public static method Main() " + body +
                  " end Main; " + members + " end Host;");
}

// Assert the program RAN to a clean exit and produced exactly `expected` on
// stdout. Split out because every row needs all three assertions: a pre-fix
// double free is a signal death, which `runAot` reports as ran == false with
// an empty-or-truncated stdout.
void expectRunOutput(const bezel::test::JitResult& r,
                     const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- #7861: the discard of an owning-choice production ----------------------

BEZEL_TEST(codegen_owning_choice_discard_and_bang_field_store_7861_7858,
           spec_17_13_a_discard_of_an_owning_choice_call_disposes_its_payload) {
    // §17.13's own rule for an owning-classified choice: the active case's
    // owned payloads are disposed at the end of the discard statement. Two
    // `Resource`s per iteration are constructed — the callee's local and the
    // caller's — and BOTH must be disposed exactly once, so the count is `2`
    // per iteration. Pre-fix: `1` per iteration (the callee's local was moved
    // out correctly; the DISCARDED one leaked) and a LeakSanitizer report.
    expectRunOutput(
        run("for var i in 0..<10 do "
            "  var own: Bundle? := Bundle.Ready(new Resource()); "
            "  _ := Host.Make(); "
            "end; "
            "Console.WriteLine($\"{Resource.Count}\");",
            "",
            "public static method Make(): Bundle "
            "  var b: Bundle := Bundle.Ready(new Resource()); "
            "  return b; "
            "end Make; "),
        std::string("20\n"));
}

BEZEL_TEST(codegen_owning_choice_discard_and_bang_field_store_7861_7858,
           spec_17_13_the_same_discard_over_the_nullable_choice_spelling) {
    // §35.9's `C?` wrapper is the same move-only kind ("For an **owning** `C`,
    // the wrapper `C?` is itself move-only"), and the discard's discharge is
    // the same §35.4 active-case walk — the `hasValue` guard decides whether
    // there is anything to dispose. The NULLABLE callee is the row that
    // exercises that guard: pre-fix it leaked exactly as the non-nullable row
    // did.
    expectRunOutput(
        run("for var i in 0..<10 do "
            "  var own: Bundle? := Bundle.Ready(new Resource()); "
            "  _ := Host.MakeOpt(); "
            "end; "
            "Console.WriteLine($\"{Resource.Count}\");",
            "",
            "public static method MakeOpt(): Bundle? "
            "  var b: Bundle? := Bundle.Ready(new Resource()); "
            "  return b; "
            "end MakeOpt; "),
        std::string("20\n"));
}

BEZEL_TEST(codegen_owning_choice_discard_and_bang_field_store_7861_7858,
           spec_35_3_a_discard_of_an_owning_choice_construction_disposes_its_payload) {
    // §35.3: "an owning construction is an `owned` move-only value ... the
    // caller takes ownership and discharges it by binding, storing, returning,
    // or consuming it". A discard is the CONSUME spelling, so the constructed
    // case's payload is disposed once — the same active-case walk, reached
    // through the same gate (a case construction IS a call, §35.3).
    expectRunOutput(
        run("_ := Bundle.Ready(new Resource()); "
            "Console.WriteLine($\"{Resource.Count}\");"),
        std::string("1\n"));
}

// --- #7858: the `!` spelling of the field store -----------------------------

BEZEL_TEST(codegen_owning_choice_discard_and_bang_field_store_7861_7858,
           spec_17_8_a_bang_read_stored_into_an_owned_choice_field_moves_the_local) {
    // §17.8: "`sink._f := x!;` consumes `x` exactly as `sink._f := x;` does".
    // So the local `o` is vacated and the FIELD owns the payload: exactly ONE
    // disposal, at the holder's destruction (`_ := h;` discharges the holder —
    // §17.13's *Discard of a named owned binding*). Pre-fix: the mark was
    // declined (the `!` wrapper is not a bare identifier), `o`'s scope-end
    // pass disposed the payload the field had taken, and the Holder's
    // destructor disposed it again — AddressSanitizer heap-use-after-free.
    expectRunOutput(
        run("var o: Bundle? := Bundle.Ready(new Resource()); "
            "var h := new Holder(); "
            "h.G := o!; "
            "_ := h; "
            "Console.WriteLine($\"{Resource.Count}\");"),
        std::string("1\n"));
}

BEZEL_TEST(codegen_owning_choice_discard_and_bang_field_store_7861_7858,
           spec_17_8_control_the_bare_read_stored_into_the_same_field) {
    // The differential control the issue names ("the plain `h.G := o` (c05) is
    // clean"): the BARE spelling was already correct and must stay correct.
    // Same single disposal, same count — the `!` spelling now matches it.
    expectRunOutput(
        run("var o: Bundle? := Bundle.Ready(new Resource()); "
            "var h := new Holder(); "
            "h.G := o; "
            "_ := h; "
            "Console.WriteLine($\"{Resource.Count}\");"),
        std::string("1\n"));
}

BEZEL_TEST(codegen_owning_choice_discard_and_bang_field_store_7861_7858,
           spec_17_8_a_bang_read_of_a_non_owning_choice_is_unchanged) {
    // The NEGATIVE CONTROL: a `!` read of a NON-owning (value-like) choice
    // carries no disposal obligation, so the count must stay `0` — the fix is
    // scoped to the owning classification and must not invent a disposal for
    // a choice that owns nothing.
    expectRunOutput(
        run("var o: Bag? := Bag.One; "
            "var x := o!; "
            "Console.WriteLine($\"{Resource.Count}\");",
            "choice Bag One, Two(v: int); end Bag; "),
        std::string("0\n"));}
