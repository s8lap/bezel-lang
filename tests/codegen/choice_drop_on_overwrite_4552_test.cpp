// Codegen regression tests for issue #4552 (the first of its four OMITTED
// disposals): §35.4 *Drop-on-overwrite is defined and automatic* and §35.9
// *Nullable `C?`*, spec v0.306.
//
// §35.4: "Whole-value reassignment of an owning-choice local or field
// *disposes the displaced value* per §28.7, like an owned class binding. A
// choice has no partial states, so the single-value form of that rule applies
// as written. … `x := null;` on an owned `C?` location likewise disposes the
// displaced value's payloads (§35.9)."
//
// §35.9: "For an **owning** `C`, the wrapper `C?` is itself move-only …
// `null` assignment into an owned-state `C?` location is drop-on-overwrite —
// the displaced value's payloads are disposed (§35.4)."
//
// THE BUG. Codegen had no drop-on-overwrite path for a choice at all. The
// class-local pass (`disposeDisplacedOwnedClassLocal`) bails at its
// `unwindOwnedLocalClass` gate for a choice-typed local — a choice is a value
// type, not a class reference — and the scope-end active-case pass
// (`registerOwningChoiceDisposal`) only ever sees whatever the slot holds
// LAST. So every value a reassignment displaced was leaked outright.
// Additionally the scope-end pass read a `C?` slot's `{ i8 hasValue, <choice
// struct> }` wrapper AS the bare choice struct, decoding `hasValue` as the
// tag, so the nullable spelling leaked even without a reassignment.
//
// Each test below states the SPEC's required observable, not the compiler's
// pre-fix one. The first two are the rule-corpus cases `s35_4_0005` and
// `s35_9_0001` verbatim (tests/conformance/rules/cases/), which is why the
// expected stdout is spelled out line by line: the omission's signature is a
// MISSING line, and disposing at the wrong moment is a DIFFERENT defect
// (#4552's ordering group) that a bare "was it disposed at all" oracle would
// not separate.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The `Resource` / `Bundle` shell shared by the ordering tests: a destructor
// that PRINTS its name, so the assertions can pin both the fact and the
// moment of each disposal.
const char* kNamedResourceDecls =
    "class Resource "
    "  private _name: string; "
    "  public constructor(name: string) _name := name; end constructor; "
    "  destructor Console.WriteLine($\"dispose {_name}\"); end destructor; "
    "end Resource; "
    "choice Bundle Ready(owned r: Resource), Empty; end Bundle; ";

// A counting shell for the loop-amplified leak oracle: a one-shot disposal
// row can pass on both sides of a leak fix, so the count is taken over many
// iterations and compared against the exact number of displaced values.
const char* kCountingResourceDecls =
    "class Resource "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Resource._n := Resource._n + 1; end destructor; "
    "end Resource; "
    "choice Bundle Ready(owned r: Resource), Empty; end Bundle; ";

std::string runOut(const std::string& decls, const std::string& body,
                   const std::string& extraMembers = "") {
    auto r = runAot(std::string("module M; ") + decls +
                    " class Host public static method Main() " + body +
                    " end Main; " + extraMembers + " end Host;");
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// --- §35.4: the displaced value is disposed AT THE STORE --------------------

BEZEL_TEST(codegen_choice_drop_on_overwrite_4552,
           spec_35_4_reassign_owning_choice_local_disposes_displaced_at_store) {
    // Rule-corpus case s35_4_0005 (§35.4, v0.306). §35.4 sequences the store:
    // "the RHS is evaluated first, then the displaced value is disposed, then
    // the new value is stored" — so "dispose old" precedes "reassigned", and
    // the discard at the end disposes "new" (§35.10's move-only discard
    // clause). Pre-fix observable was "reassigned\ndispose new": the displaced
    // value was not disposed at all, not merely disposed late.
    BEZEL_EXPECT_EQ(
        runOut(kNamedResourceDecls,
               "var b := Bundle.Ready(new Resource(\"old\")); "
               "b := Bundle.Ready(new Resource(\"new\")); "
               "Console.WriteLine(\"reassigned\"); "
               "_ := b;"),
        std::string("dispose old\nreassigned\ndispose new\n"));
}

BEZEL_TEST(codegen_choice_drop_on_overwrite_4552,
           spec_35_4_reassign_to_payload_free_case_still_disposes_displaced) {
    // The same rule with a payload-FREE new value: §35.4's active-case
    // disposal disposes nothing for `Empty`, but the rule under test is about
    // the DISPLACED value, which is `Ready(old)` either way. Pre-fix this
    // leaked silently — the scope-end pass saw only the final `Empty`.
    BEZEL_EXPECT_EQ(
        runOut(kNamedResourceDecls,
               "var b := Bundle.Ready(new Resource(\"old\")); "
               "b := Bundle.Empty; "
               "Console.WriteLine(\"reassigned\");"),
        std::string("dispose old\nreassigned\n"));
}

// --- §35.9: `x := null` on an owned-state `C?` ------------------------------

BEZEL_TEST(codegen_choice_drop_on_overwrite_4552,
           spec_35_9_null_into_owned_state_optional_disposes_displaced) {
    // Rule-corpus case s35_9_0001 (§35.9, v0.306): "`null` assignment into an
    // owned-state `C?` location is drop-on-overwrite — the displaced value's
    // payloads are disposed". Pre-fix observable was "nulled" alone.
    BEZEL_EXPECT_EQ(
        runOut("class Widget "
               "  public constructor() end constructor; "
               "  destructor Console.WriteLine(\"widget disposed\"); "
               "  end destructor; "
               "end Widget; "
               "choice Outcome Fetched(owned w: Widget), Failed; end Outcome; ",
               "var o: Outcome? := Outcome.Fetched(new Widget()); "
               "o := null; "
               "Console.WriteLine(\"nulled\");"),
        std::string("widget disposed\nnulled\n"));
}

BEZEL_TEST(codegen_choice_drop_on_overwrite_4552,
           spec_35_9_optional_owning_choice_local_disposes_at_scope_end) {
    // The §35.9 wrapper's OTHER consequence, and the reason the scope-end pass
    // had to move to the location-aware helper in the same change: `C?` is
    // "the ordinary nullable value type (§7.2)", whose storage is
    // `{ i8 hasValue, <choice struct> }`. Reading that wrapper's leading bytes
    // as the tag decoded `hasValue` (1) as the tag, selected the payload-free
    // `Failed` case (index 1), and disposed nothing. §35.4's "locals are
    // auto-disposed at scope end" applies to the `C?` spelling too.
    BEZEL_EXPECT_EQ(
        runOut("class Widget "
               "  public constructor() end constructor; "
               "  destructor Console.WriteLine(\"widget disposed\"); "
               "  end destructor; "
               "end Widget; "
               "choice Outcome Fetched(owned w: Widget), Failed; end Outcome; ",
               "var o: Outcome? := Outcome.Fetched(new Widget()); "
               "Console.WriteLine(\"before end\");"),
        std::string("before end\nwidget disposed\n"));
}

// --- The loop-amplified leak oracle -----------------------------------------

BEZEL_TEST(codegen_choice_drop_on_overwrite_4552,
           spec_35_4_every_displaced_value_in_a_loop_is_disposed) {
    // A one-shot disposal row is a weak oracle for a LEAK fix. Amplify: 500
    // reassignments displace 500 values, and the 501st (the final slot
    // content) is disposed by the scope-end active-case pass — so the spec's
    // count is exactly 501. Pre-fix the count was 1.
    BEZEL_EXPECT_EQ(
        runOut(kCountingResourceDecls,
               // The loop lives in its own method so the scope-end disposal of
               // the final slot content has already run when the count prints.
               "Host.Churn(); Console.WriteLine(Resource._n);",
               "public static method Churn() "
               "  var b := Bundle.Ready(new Resource()); "
               "  var i := 0; "
               "  while i < 500 do "
               "    b := Bundle.Ready(new Resource()); "
               "    i := i + 1; "
               "  end while; "
               "end Churn;"),
        // 500 displaced + 1 scope-end disposal.
        std::string("501\n"));
}

// --- §28.7's two suppressions: neither store may double-free ----------------

BEZEL_TEST(codegen_choice_drop_on_overwrite_4552,
           spec_28_7_self_assignment_of_owning_choice_local_disposes_once) {
    // §28.7 runs the drop-on-overwrite pass only "if the previous value is
    // live". `b := b;` stores the same bytes back, so disposing the
    // "displaced" value would free the payload the store is about to write —
    // a double-free at scope end. Exactly one disposal is the spec answer.
    BEZEL_EXPECT_EQ(
        runOut(kNamedResourceDecls,
               "var b := Bundle.Ready(new Resource(\"self\")); "
               "b := b; "
               "Console.WriteLine(\"self done\");"),
        std::string("self done\ndispose self\n"));
}

BEZEL_TEST(codegen_choice_drop_on_overwrite_4552,
           spec_28_7_moved_from_owning_choice_local_stores_without_disposal) {
    // §28.7: "A moved-from or null location therefore stores without
    // disposal." `b := Pass(b);` hands the value out and takes it back
    // through the same slot, so the location is moved-from AT the store and
    // the displaced-value pass must not run — otherwise the value the callee
    // just returned is freed under the store. §15.3's *Whole-value
    // reassignment* names this shape verbatim: "A reassignment whose RHS
    // consumes the LHS — self-assignment `arr := arr;`, `arr := f(arr)` (a
    // by-value-move parameter, this section's *Parameter passing* bullet) …
    // the RHS move leaves the location moved-from before the pass runs".
    //
    // The argument is written UNMARKED, and that is the load-bearing spelling:
    // §15.3's *Parameter passing* bullet says a move-only V's argument "is
    // written unmarked — `Take(arr)`, not `Take(move arr)` — including where
    // it is a named binding the call leaves moved-from … so `move` at such an
    // argument is rejected as at any marker-free position". A `move` marker
    // here is the §22.3 error the first version of this row carried (it was
    // authored 2026-10-02, before #5388's v0.314 gate landed the diagnostic
    // that rejects it) — the compiler is right to reject it and the ROW was
    // wrong. MEASURED pre-fix on the corrected spelling: the call left the
    // caller's `b` live, so `b`'s scope-end active-case pass disposed the
    // payload the returned value already owned — `dispose b` / `rewrapped` /
    // then SIGSEGV, and `AddressSanitizer: heap-use-after-free ... READ of
    // size 8` under `--sanitize=address`. One disposal, at scope end, on the
    // value `b` holds when the construct exits.
    BEZEL_EXPECT_EQ(
        runOut(kNamedResourceDecls,
               "var b := Bundle.Ready(new Resource(\"b\")); "
               "b := Host.Pass(b); "
               "Console.WriteLine(\"rewrapped\");",
               "public static method Pass(v: Bundle): Bundle => v;"),
        std::string("rewrapped\ndispose b\n"));
}
