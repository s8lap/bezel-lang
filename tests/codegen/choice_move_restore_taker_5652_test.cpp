// Codegen regression test for #5652, the TAKER half: a WHOLE-VALUE MOVE out of
// an OWNING CHOICE field followed by the §28.7 RESTORATION STORE,
// `var t := move h.C; h.C := t;`, spec v0.334.
//
// This is the third piece of a three-part fix. The two siblings are already in
// the tree and are NOT re-tested here:
//   * `a5b42f36` — the LOCAL-source spelling. `markOwningChoiceMovedFromCore`
//     failed to unwrap the `ast::UnaryExpr{op="move"}` wrapping the identifier,
//     so `var t := move src;` left BOTH slots owning the payload.
//   * `d2a871ca` (staging) — the FIELD-source spelling. `lowerUnary`'s
//     move-field guard had no owning-CHOICE predicate at all, so the FIELD
//     kept its disposal while the taker took one. Its mark is a whole-struct
//     zero-store, adopting the rationale recorded at
//     LlvmBackend_Disposal.cpp:6938 ("WHY NOT A SENTINEL TAG… Zero is an
//     in-range, constructible state").
//
// WHAT IS LEFT, AND WHY `d2a871ca` STRUCTURALLY CANNOT REACH IT. The
// restoration idiom is
//     var t := move h.C;   // the field is vacated; the TAKER owns the payload
//     h.C := t;            // §28.7 restoration store
// That store lands in `lowerOwnedChoiceFieldStore`
// (LlvmBackend_Setup.cpp:32101), which ran §28.7 drop-on-overwrite and then
// `CreateStore(newVal, fieldAddr)` — without VACATING `t`. The field's mark
// from `d2a871ca` is on the FIELD's storage; the taker here is a LOCAL, so no
// arm of that fix reaches it. Both `t`'s scope-end active-case pass
// (registerOwningChoiceDisposal) and the field's §28.4 step-2 pass
// (emitOwnedFieldDisposals → emitOwningChoiceLocationDisposal) then dispatched
// `Dispose` through the same payload pointer.
//
// THE SPEC TEXT THIS FIX RESTS ON, quoted:
//   §28.7 (drop-on-overwrite): "A moved-from or null location therefore stores
//   without disposal — which is precisely how the field-restoration rule's
//   `move`-then-reassign idiom (§28.8.1) stays single-disposal."
//   §28.8.1 *The vacated location*: the vacated location "is in §9.4's
//   **disposed state**, holding no live value"; "every later read faults again"
//   and the value "belongs to the taker, which disposes it once".
//   §15.3 *Move-only value types*, *Rules* bullet 1: "Assignment transfers the
//   value from source to destination, leaving the source in moved-from state."
//   §15.3 *Whole-value reassignment*: "The new value arrives with its own
//   per-slot states — `a := b;` where `b` (a whole-value move) carries
//   moved-from slots leaves `a` carrying them, with `b`'s restoration
//   obligations transferred to `a`."  ← the taker's obligations move WITH the
//   value, so its scope-end pass must not also fire.
//   §15.3 *Local lifetime*: a whole-value move "leaves the local holding no
//   value rather than a hole, so no pass walks it (K1's moved-from state:
//   'discharge machinery skips it')".
//   §9.4 *Move-only receivers*: "A field that a checked-mode fault leaves in
//   the state … is therefore no hole: its value belongs to the taker alone,
//   which disposes it once."
//
// THE ASYMMETRY, AND WHY THE ORACLE DISTINGUISHES ITS TWO DIRECTIONS.
// Marking a slot the store did NOT adopt would LEAK the payload — its
// destructor line is simply ABSENT. Failing to mark one DOUBLE-FREES — its
// line is DUPLICATED, and the second dispatch reads freed memory. A one-shot
// "did it print" oracle cannot separate those from a crash, so the loop row
// below asserts an exact disposal COUNT over 200 iterations with a counting
// destructor: too low means a leak, too high a double free, exact means the
// transfer is exactly-once. The one-shot rows below use a destructor that
// PRINTS its name so the same distinction is visible line by line.
//
// SCOPE — WHAT THIS FILE DOES NOT CLAIM. The READ-side §9.4 disposed-state
// ENCODING is separate work: a zeroed field does not *fault*, so the
// borrowing-match read inside the restoration window still prints `True`
// where `faulted` is expected. That is filed separately (`98dee644`, branch
// `r28-5652-read`, NOT on this base) and is deliberately not attempted here.
// Consequently conformance rows `s9_4_0008` / `s9_4_0010` / `s15_1_1_0009`
// remain red and are NOT claimed by this file; no `expect:`/`diagnostic:`
// column, `manifest.tsv` or `baseline.tsv` row is touched.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A destructor that PRINTS: one line per disposal, so a doubled disposal is a
// duplicated LINE and a suppressed one an ABSENT line, both distinguishable
// from the spec's answer. Same shell as the field-source sibling
// (choice_move_keyword_field_5652_test.cpp), so the two files' outputs are
// directly comparable.
const char* kNamedResourceDecls =
    "class Resource "
    "  private _name: string; "
    "  public constructor(name: string) _name := name; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_name}\"); end destructor; "
    "end Resource; "
    "choice Slot Full(owned r: Resource), Empty; end Slot; "
    "class Holder "
    "  public C: Slot; "
    "  public constructor() C := Slot.Full(new Resource(\"c\")); "
    "  end constructor; "
    "end Holder; ";

// A COUNTING destructor for the loop-amplified row: only an exact count over
// many iterations separates "disposed twice" from "disposed once" from
// "never disposed".
const char* kCountingResourceDecls =
    "class Resource "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Resource._n := Resource._n + 1; end destructor; "
    "end Resource; "
    "choice Slot Full(owned r: Resource), Empty; end Slot; "
    "class Holder "
    "  public C: Slot; "
    "  public constructor() C := Slot.Full(new Resource()); "
    "  end constructor; "
    "end Holder; ";

bezel::test::JitResult run(const std::string& decls, const std::string& body) {
    return runAot(std::string("module M; ") + decls +
                  " class Host public static method Main() " + body +
                  " end Main; end Host;");
}

// All three assertions are load-bearing: a pre-fix double free is a signal
// death reported as ran == false with a truncated stdout, so an exit-code-only
// assertion would read the crash as green.
void expectRunOutput(const bezel::test::JitResult& r,
                     const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- THE PIN: the §28.7 restoration store vacates the TAKER ------------------

BEZEL_TEST(codegen_choice_move_restore_taker,
           spec_28_7_restoration_store_vacates_the_local_taker) {
    // `h.C := t` is the §28.7 restoration store of the field-restoration rule
    // (§28.8.1). Pre-fix: `restored`, `dispose c` from `t`'s scope-end pass,
    // then the holder's §28.4 step-2 pass dispatches through the SAME payload
    // — `AddressSanitizer: heap-use-after-free, READ of size 8 in
    // bz$M.Holder$DisposeCore`, region freed by `t` and allocated in the field
    // initializer. Post-fix: `t` is vacated, the FIELD is the sole owner, and
    // the payload is disposed exactly once.
    expectRunOutput(
        run(kNamedResourceDecls,
            "using var h := new Holder(); "
            "var t := move h.C; "
            "h.C := t; "
            "Console.WriteLine(\"restored\");"),
        std::string("restored\ndispose c\n"));
}

// --- the LOOP row: an exact disposal count, not a one-shot exit -------------

BEZEL_TEST(codegen_choice_move_restore_taker,
           spec_28_7_restoration_store_disposes_exactly_once) {
    // 200 iterations, counting disposals. This is the row that separates the
    // two directions of the asymmetry: the TAKER is marked, so the field alone
    // owns the payload and the count is exactly 200. Marking nothing would
    // print 400 (a double free); marking a slot the store did not adopt would
    // print fewer than 200 (a leak). A one-shot run cannot make that
    // distinction — a doubled disposal can land in still-mapped memory.
    //
    // MEASURED by hand on the base at r23-c28 (d2a871ca), ASan on the
    // generated code: the double free fires on the FIRST iteration (the
    // abort precedes the count print), so the pre-fix observable is
    // ran == false, not "400". 200 is both the pre-fix doubled count and the
    // correct post-fix one, since every iteration builds a fresh pair and
    // nothing else in the loop changes.
    expectRunOutput(
        run(kCountingResourceDecls,
            "for var i in 0..<200 do "
            "  using var h := new Holder(); "
            "  var t := move h.C; "
            "  h.C := t; "
            "end for; "
            "Console.WriteLine($\"{Resource._n}\");"),
        std::string("200\n"));
}

// --- the mark is a VACANCY: a fresh store into the TAKER re-arms it ----------

BEZEL_TEST(codegen_choice_move_restore_taker,
           spec_28_7_fresh_store_re_arms_the_vacated_taker) {
    // The §28.7 *fresh store* twin of the field-side row the sibling file
    // pins (`spec_28_7_fresh_store_re_arms_the_vacated_field`). If the fix had
    // DISABLED the taker's scope-end pass outright instead of vacating the
    // slot, this row would print only `dispose c` and lose `dispose t2` — that
    // is, it would trade the double free for a leak. §28.7's *Moved-from and
    // null* clause makes the next store re-arm the location with a live value:
    // "A moved-from or null location therefore stores without disposal".
    //
    // Order is the spec's, not an accident: `t` is declared after `h`, and
    // scope-end disposal runs in reverse declaration order, so `t`'s pass fires
    // before the holder's §28.4 step-2 pass. Two payloads, two lines, once each.
    expectRunOutput(
        run(kNamedResourceDecls,
            "using var h := new Holder(); "
            "var t := move h.C; "
            "h.C := t; "
            "t := Slot.Full(new Resource(\"t2\")); "
            "Console.WriteLine(\"rearmed\");"),
        std::string("rearmed\ndispose t2\ndispose c\n"));
}

// --- the NULLABLE `C?` spelling: the same transfer, a different storage -------

BEZEL_TEST(codegen_choice_move_restore_taker,
           spec_28_7_restoration_store_vacates_the_taker_for_a_nullable_field) {
    // §35.9's `C?` wrapper lowers to `{ i8 hasValue, <choice struct> }`, so the
    // TAKER's slot is a DIFFERENT LLVM type from the bare-`C` rows above and
    // the mark's `want` computation takes its other branch. The transfer is the
    // same one, so the answer is the same: the field ends up the sole owner and
    // the payload is disposed exactly once. §35.9 *Choice nullability* admits
    // the wrapper at a field, and §28.7's drop-on-overwrite covers it because
    // `isOwnedChoiceFieldLvalue` classifies through the SAME
    // `classifyChoiceType` call for the nullable and non-nullable spellings —
    // the two cannot drift apart.
    //
    // MEASURED on this base with the fix applied, ASan on the generated code:
    // exactly `restored` / `dispose c`, exit 0, no sanitizer report.
    expectRunOutput(
        run(std::string(
                "class Resource "
                "  private _name: string; "
                "  public constructor(name: string) _name := name; end constructor; "
                "  destructor Console.WriteLine($\"dispose {_name}\"); end destructor; "
                "end Resource; "
                "choice Slot Full(owned r: Resource), Empty; end Slot; "
                "class Holder "
                "  public C: Slot?; "
                "  public constructor() C := Slot.Full(new Resource(\"c\")); "
                "  end constructor; "
                "end Holder; "),
            "using var h := new Holder(); "
            "var t := move h.C; "
            "h.C := t; "
            "Console.WriteLine(\"restored\");"),
        std::string("restored\ndispose c\n"));
}

// --- the LEAK control: a store that did NOT adopt must not mark -------------

BEZEL_TEST(codegen_choice_move_restore_taker,
           spec_28_7_a_fresh_rhs_store_does_not_vacate_the_field) {
    // THE DIFFERENTIAL CONTROL, and it distinguishes BOTH directions of the
    // asymmetry at once. The RHS of the last store is a FRESH case
    // construction, not a local taker, so there is no `t` to vacate; what that
    // store displaces is the FIELD's current value — `c`, put there by the
    // restoration store above — which §28.7's drop-on-overwrite disposes
    // exactly once.
    //
    //   * If the fix marked a slot the arm did NOT adopt (the field, here),
    //     `dispose c` would VANISH and this row would fail — a LEAK.
    //   * If the taker `t` were still owning `c`, the store's drop-on-overwrite
    //     would free it and `t`'s scope-end pass would dispose it AGAIN at
    //     scope end — the double free, and this row would crash under ASan.
    //
    // It is green on the base and must STAY green.
    //
    // THE ORDER IS §28.7's, and it is MEASURED, not assumed: "The right-hand
    // side is evaluated first; then, if the previous value is live (non-null
    // and not moved-from …), it is disposed; then the new value is stored."
    // `dispose c` therefore lands AT the store — before `rearmed2` — because
    // the displaced value dies at the overwrite point (§35.4 *Drop-on-overwrite
    // is defined and automatic*), not at some later scope end. `c2` is owned by
    // the field and disposed by the holder's §28.4 step-2 pass afterwards.
    //
    // MEASURED on this base with the fix applied, ASan on the generated code:
    // exactly `dispose c` / `rearmed2` / `dispose c2`, exit 0, no sanitizer
    // report. (This row's expectation was corrected from a first guess that
    // placed both disposals at scope end; the drop-on-overwrite runs at the
    // store, and the probe output is what settled it.)
    expectRunOutput(
        run(kNamedResourceDecls,
            "using var h := new Holder(); "
            "var t := move h.C; "
            "h.C := t; "
            "h.C := Slot.Full(new Resource(\"c2\")); "
            "Console.WriteLine(\"rearmed2\");"),
        std::string("dispose c\nrearmed2\ndispose c2\n"));
}