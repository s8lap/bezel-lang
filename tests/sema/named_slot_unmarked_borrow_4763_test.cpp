// Spec-derived tests for §19.3 "Named-slot bindings and ownership" (v0.334,
// #4763) — the §35.5 rule that an UNMARKED named slot never transfers.
//
// §19.3, "Named-slot bindings and ownership":
//   "An **unmarked** named slot never transfers: a getter may not return a
//    fresh owned value (§28.6), so no slot read can mint an obligation, and in
//    an **owning match** an owned or move-only value payload matched by an
//    unmarked named slot is borrowed during the arm and disposed at construct
//    completion by the standard §35.5 machinery, exactly as a discarded slot."
//
// §35.5 states the same slot in its un-transferred remainder: "The
// un-transferred remainder — unlisted fields, unmarked named slots, discarded
// siblings — is disposed at construct completion".
//
// §15.3 restates the split from the other side: "a case-payload of move-only
// value type matched by a **positional slot or a marked named slot** instead
// **transfers into its binding at arm selection** …; an **unmarked named
// slot** keeps the borrow (§19.3)".
//
// THE DEFECT (#4763, conformance row s35_5_0010). #2197 made the named-slot
// FORM parseable and normalized it — REORDERING the named slots into
// declaration order and CLEARING `slotName`, because every consumer indexes
// `subPatterns[i]` positionally. That erasure is what created the hole: with
// `slotName` gone, `collectOwningMatchTransferBindings` (ownership) and
// `markOwningMatchTransferredSlots` (codegen) both read
// `Fetched(response = var r)` as the POSITIONAL `Fetched(var r)` sitting at the
// same index, and a positional slot DOES transfer. So the binding was admitted
// as an owned local of the arm body's scope and `move r` was accepted. The
// ownership class of a named slot is keyed on its SPELLING, so `PositionalPattern`
// now carries a `namedSlot` flag that survives normalization, and both passes
// consult it.
//
// s35_5_0010 was banked `pass` only because the program used to fail to PARSE,
// so the `reject` expectation was satisfied by a syntax error. It became a live
// REGRESSION the moment #2197 made the form parse.
//
// MANDATORY DIFFERENTIAL CONTROLS. Every row here that changes a verdict is
// paired with the POSITIONAL spelling of the same program, which is the entire
// pre-existing surface. A positional slot transfers and must keep doing so;
// these controls go red first if this change over-reached.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"          // runAot — the real bezelc AOT oracle
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

bool outContains(const bezel::test::JitResult& r, const char* sub) {
    return r.stdoutText.find(sub) != std::string::npos;
}

// An owning choice `Outcome` whose `Fetched` case carries an `owned` class
// payload, so an owning match over `client.Fetch()` DOES transfer — the only
// setting in which the named/positional split is observable. `arm` is spliced
// in verbatim so each row differs from its control by exactly one token.
std::string owningChoiceProgram(const char* arm) {
    return std::string(
               "module Conf; "
               "public class Widget "
               "  public constructor() end constructor; "
               "  destructor Console.WriteLine(\"widget disposed\"); "
               "    end destructor; "
               "end Widget; "
               "public choice Outcome "
               "  Fetched(owned response: Widget), "
               "  Failed(status: int); "
               "end Outcome; "
               "public class Client "
               "  public constructor() end constructor; "
               "  public method Fetch(): Outcome => "
               "    Outcome.Fetched(new Widget()); "
               "end Client; "
               "public class Program "
               "  public static method Main() "
               "    using var client := new Client(); "
               "    case client.Fetch() of ") +
           arm +
           "    end case; "
           "    Console.WriteLine(\"after\"); "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// ===========================================================================
// THE PINNED ROW — conformance case s35_5_0010's program, verbatim.
//
// RED WITHOUT THE FIX: on the #2197 compiler this COMPILES (the corpus reports
// `REGRESSION s35_5_0010 reject compiled, but the spec rejects this`). The
// binding `r` was classified as an owning-match transfer, so `move r` was an
// ordinary transfer of an owned local.
//
// The diagnostic needle `never transfers` is the corpus row's own authored
// `diagnostic:` — §19.3's sentence, "An unmarked named slot never transfers".
// ===========================================================================
BEZEL_TEST(named_slot_unmarked_borrow_4763,
           unmarked_named_slot_binding_cannot_be_moved_spec_19_3_35_5) {
    auto r = runAot(owningChoiceProgram(
        "Fetched(response = var r) => do "
        "  using var w := move r; "
        "  Console.WriteLine(\"adopted\"); "
        "end, "
        "Failed(_) => Console.WriteLine(\"failed\") "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "never transfers"));
}

// The rule names §19.3 and §35.5 explicitly; a diagnostic that merely rejected
// the program would satisfy the row while leaving the reader with no rule to
// look up.
BEZEL_TEST(named_slot_unmarked_borrow_4763,
           rejection_names_both_sections_spec_19_3_35_5) {
    auto r = runAot(owningChoiceProgram(
        "Fetched(response = var r) => do "
        "  using var w := move r; "
        "end, "
        "Failed(_) => Console.WriteLine(\"failed\") "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "19.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "35.5"));
}

// ===========================================================================
// DIFFERENTIAL CONTROL — the POSITIONAL spelling of the row above. §35.5: a
// whole-slot binding of an `owned` payload in an owning match IS transferred
// into its binding, and `move r` on it is admitted. If this control ever goes
// red, the fix over-reached and closed the positional transfer as well.
// ===========================================================================
BEZEL_TEST(named_slot_unmarked_borrow_4763,
           positional_slot_still_transfers_differential_control) {
    auto r = runAot(owningChoiceProgram(
        "Fetched(var r) => do "
        "  using var w := move r; "
        "  Console.WriteLine(\"adopted\"); "
        "end, "
        "Failed(_) => Console.WriteLine(\"failed\") "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outContains(r, "adopted"));
    BEZEL_EXPECT_TRUE(outContains(r, "widget disposed"));
}

// ===========================================================================
// THE OTHER HALF OF THE RULE — "disposed at construct completion, exactly as a
// discarded slot".
//
// With no `move` at all, `response = var r` binds a BORROW and the payload is
// reclaimed by the construct-completion pass. The pre-fix compiler instead
// classified the slot as a transfer and marked the payload as adopted —
// leaving it owned by a binding nobody discharges. MEASURED pre-fix: a 16-byte
// LeakSanitizer report and NO "widget disposed" line at all, so the destructor
// assertion below is the row's oracle and it is red without the fix.
//
// The generated-code ASan net for this row (which catches the OTHER error
// direction of the codegen skip — failing to mark an adopted slot double-frees)
// is the suite-wide forced sweep
// (`BEZEL_AOT_FORCE_SANITIZE=address scripts/agent_check.sh …`), not a
// per-row `runAotChecked`: a checked call whose only claims run through a
// JitResult-taking in-file predicate is counted as a new toolchain-SKIP
// absorbing site by `scripts/check_toolchain_skip_surface.py`, and
// `absorbing_sites` may only FALL. `r.ran` plus two positive stdout claims
// below is the anti-skip posture that row actually needs.
//
// The POSITIONAL spelling of this same program is the differential control
// below.
// ===========================================================================
BEZEL_TEST(named_slot_unmarked_borrow_4763,
           unmarked_named_slot_payload_disposed_at_construct_completion) {
    auto r = runAot(owningChoiceProgram(
        "Fetched(response = var r) => Console.WriteLine(\"read only\"), "
        "Failed(_) => Console.WriteLine(\"failed\") "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outContains(r, "read only"));
    // The §19.3/§35.5 disposal point: the construct-completion reclaim, not an
    // arm-binding discharge.
    BEZEL_EXPECT_TRUE(outContains(r, "widget disposed"));
}

// DIFFERENTIAL CONTROL for the row above. The POSITIONAL binding is an OWNED
// local of the arm body's scope under §35.5, so an arm that never discharges
// it is the §28.3 must-use case — corpus row s35_5_0031, a banked divergence
// that predates and is independent of this rule. This row pins only that the
// positional spelling's ACCEPTANCE is untouched; it deliberately does NOT
// assert the absence of a leak, because that is #4626/s35_5_0031's work and
// pinning "it leaks" here would freeze the divergence into a green row.
BEZEL_TEST(named_slot_unmarked_borrow_4763,
           positional_read_only_acceptance_unchanged_differential_control) {
    auto r = runAot(owningChoiceProgram(
        "Fetched(var r) => Console.WriteLine(\"read only\"), "
        "Failed(_) => Console.WriteLine(\"failed\") "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outContains(r, "read only"));
}

// ===========================================================================
// The rule covers a MOVE-ONLY VALUE payload too — §15.3 names both kinds, and
// they reach a DIFFERENT rejection map (`borrowOnlyMoveOnlyArmBindings_`).
// A nested owning choice is the move-only payload with a pattern surface.
// ===========================================================================
BEZEL_TEST(named_slot_unmarked_borrow_4763,
           unmarked_named_slot_over_move_only_payload_cannot_be_moved) {
    auto r = runAot(
        "module Conf; "
        "public class Widget4763 "
        "  public constructor() end constructor; "
        "end Widget4763; "
        "public choice Slot4763 Empty4763, Full4763(owned held: Widget4763); "
        "end Slot4763; "
        "public choice Outer4763 None4763, Has4763(owned inner: Slot4763); "
        "end Outer4763; "
        "public class Program "
        "  public static method Main() "
        "    case Outer4763.Has4763("
        "      Slot4763.Full4763(new Widget4763())) of "
        "      Has4763(inner = var h) => do "
        "        using var w := move h; "
        "      end, "
        "      None4763 => () "
        "    end case; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "never transfers"));
}

// DIFFERENTIAL CONTROL for the row above: the POSITIONAL spelling of the same
// move-only payload. §15.3: a move-only value payload matched by a positional
// slot "transfers into its binding at arm selection exactly as an `owned`
// payload does".
BEZEL_TEST(named_slot_unmarked_borrow_4763,
           positional_move_only_slot_still_transfers_differential_control) {
    auto r = runAot(
        "module Conf; "
        "public class Widget4763 "
        "  public constructor() end constructor; "
        "end Widget4763; "
        "public choice Slot4763 Empty4763, Full4763(owned held: Widget4763); "
        "end Slot4763; "
        "public choice Outer4763 None4763, Has4763(owned inner: Slot4763); "
        "end Outer4763; "
        "public class Program "
        "  public static method Main() "
        "    case Outer4763.Has4763("
        "      Slot4763.Full4763(new Widget4763())) of "
        "      Has4763(var h) => do "
        "        using var w := move h; "
        "        Console.WriteLine(\"adopted\"); "
        "      end, "
        "      None4763 => () "
        "    end case; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outContains(r, "adopted"));
}

// ===========================================================================
// A named slot over a RECORD head is untouched: §19.3's rule is about a case
// payload that could carry an obligation, and a record field binds a copy or an
// ordinary borrow either way. This is conformance row s19_3_0007's own program.
// ===========================================================================
BEZEL_TEST(named_slot_unmarked_borrow_4763,
           record_named_slot_unaffected_control_spec_19_3) {
    auto r = runAot(
        "module M; "
        "public record Rect4763(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var rect := new Rect4763(3, 4); "
        "    case rect of "
        "      Rect4763(Width = 3, Height = var h) => "
        "        Console.WriteLine(h.ToString()), "
        "      _ => Console.WriteLine(\"other\") "
        "    end case; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outContains(r, "4"));
}