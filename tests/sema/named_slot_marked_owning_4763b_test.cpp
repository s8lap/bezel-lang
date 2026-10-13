// Spec-derived tests for §19.3 "Named-slot bindings and ownership" (v0.334)
// — the §35.5 rule that a MARKED named slot (`move N = var x`) DOES transfer.
// v0.338 (proposal 0183) RETIRED the marked slot: every row whose program used
// the marker now pins the parser rejection "`move` is not a slot spelling"
// (§19.3, §35.5 v0.338). The positional controls below are unchanged.
// This is the follow-up #4763 named: its two skip sites
// (`collectOwningMatchTransferBindings`, `markOwningMatchTransferredSlots`)
// both carried "whoever implements that admission must revisit this line".
//
// §19.3, "Named-slot bindings and ownership":
//   "A named slot may instead carry the transfer marker on its **source**:
//    `move N = var x`. The marked slot is admitted exactly when **(i)** the
//    match is an **owning match** (§35.5 — either trigger), **(ii)** `N` names
//    an **`owned`-marked payload field or a payload field of move-only value
//    type** of the matched case …, and **(iii)** the slot's pattern is a
//    top-level binding (`var x`; v1). Any number of slots in one list may be
//    marked: the admission conditions and the transfer semantics are per-slot
//    and compose — the positional form already transfers *every*
//    obligation-bearing slot at once, so multiple named transfers spell a
//    subset of what the construct already does. On arm selection each marked
//    payload transfers into its binding exactly as the corresponding positional
//    slot would: `x` is an owned local of the arm body's scope under §35.5's
//    standard arm-binding rules, and unmarked named slots in the same list keep
//    the borrow reading above."
//
// §19.3's marker refusals, each one a row below:
//   "The marker anywhere else is a compile error naming the failed condition:
//    on a slot of a **borrowing match** …; on a **copyable value, `string`, or
//    runtime-managed payload** …; on a **plain (unmarked) class- or
//    interface-typed payload field** of an owning case …; and on a **record or
//    class** pattern head — by derivation, not fiat … A **`_` right-hand side**
//    (`move N = _`) is already inadmissible under condition (iii) …"
//
// §15.3 names the same two spellings from the move-only side: "a case-payload of
// move-only value type matched by a **positional slot or a marked named slot**
// instead **transfers into its binding at arm selection**".
//
// MEMORY-SAFETY, NOT DIAGNOSTICS. #4763 recorded the asymmetry that governs
// every skip line here: "the two error directions of this function are not
// symmetric … failing to mark a slot the arm adopted is a DOUBLE FREE, while
// marking one it did not is a leak." So every acceptance row below asserts a
// DISPOSAL COUNT, not merely that the program compiles, and every one is LOOPED
// 200x — a one-shot leak repro passes vacuously.
//
// MANDATORY DIFFERENTIAL CONTROLS. Every changed verdict is paired with the
// POSITIONAL spelling of the same program, which is the entire pre-existing
// surface. These go red first if the change over-reached.

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

// How many times `needle` occurs in the program's stdout. This is the disposal
// oracle: a leaked payload never runs its destructor, so its line is absent.
int countOut(const bezel::test::JitResult& r, const char* needle) {
    int n = 0;
    std::size_t pos = 0;
    while ((pos = r.stdoutText.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += std::strlen(needle);
    }
    return n;
}

// The ITERATION COUNT of every looped row below. 200 iterations of a two-widget
// dispose is 400 destructor lines — a total a leak or a double free moves off
// by a visible amount, which a one-shot repro cannot show.
constexpr int kIters = 200;

// An owning choice `Outcome` whose `Fetched` case carries `payloadCount`
// `owned` class payloads, so an owning match over `client.Fetch()` DOES
// transfer — the only setting in which the named/positional split is
// observable. Each Widget prints one "widget disposed" line from its
// destructor, which is what the disposal oracle counts.
std::string owningChoice(int payloadCount) {
    std::string payloads = "Fetched(";
    for (int k = 0; k < payloadCount; ++k) {
        if (k) payloads += ", ";
        payloads += std::string("owned p") + std::to_string(k) + ": Widget";
    }
    payloads += "), Failed(status: int)";
    std::string args = "Outcome.Fetched(";
    for (int k = 0; k < payloadCount; ++k) {
        if (k) args += ", ";
        args += "new Widget()";
    }
    args += ")";
    return "module Conf; "
           "public class Widget "
           "  public constructor() end constructor; "
           "  destructor Console.WriteLine(\"widget disposed\"); "
           "    end destructor; "
           "end Widget; "
           "public choice Outcome " + payloads + "; end Outcome; "
           "public class Client "
           "  public constructor() end constructor; "
           "  public method Fetch(): Outcome => " + args + "; "
           "end Client; ";
}

// The looped driver: `kIters` owning matches over a fresh temporary, each with
// the given arm spliced in.
std::string loopedProgram(int payloadCount, const std::string& arm) {
    return owningChoice(payloadCount) +
           "public class Program "
           "  public static method Main() "
           "    using var client := new Client(); "
           "    for var i in 0..<200 do "
           "      case client.Fetch() of " + arm +
           "      end case; "
           "    end; "
           "    Console.WriteLine(\"done\"); "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// ===========================================================================
// THE PINNED ROW — conformance case s35_5_0008's program, verbatim modulo the
// payload field's name (`response` → `p0`, so it fits `owningChoice`).
//
// RED WITHOUT THE FIX: on the pre-fix compiler this does not compile at all.
// `normalizeNamedDestructureSlots` rejected `hasMoveMarker` on ANY named slot
// before it resolved a single name (MEASURED on the base compiler: "`move`
// marker on named slot `response =` in pattern `Fetched(...)` is admitted only
// on a choice case head …"), and the arm's `using var w := move r;` was
// independently refused with the §19.3 "an UNMARKED named slot never transfers"
// message — because the binding was read as the POSITIONAL `Fetched(var r)`
// sitting at the same index. Two diagnostics for one program.
//
// The row's authored `output:` is `adopted / widget disposed / after`, in that
// order. "widget disposed" is the §35.5 half: the payload was ADOPTED by the
// arm (`using var w`), so its disposal belongs to the arm's scope exit — which
// is only reachable if the slot was marked and the arm really does own it.
// ===========================================================================
BEZEL_TEST(named_slot_marked_owning_4763b,
           marked_named_slot_is_rejected_spec_19_3_35_5_v0_338) {
    auto r = runAot(owningChoice(1) +
        "public class Program "
        "  public static method Main() "
        "    using var client := new Client(); "
        "    case client.Fetch() of "
        "      Fetched(move p0 = var r) => do "
        "        using var w := move r; "
        "        Console.WriteLine(\"adopted\"); "
        "      end, "
        "      Failed(_) => Console.WriteLine(\"failed\") "
        "    end case; "
        "    Console.WriteLine(\"after\"); "
        "  end Main; "
        "end Program; ");
    // v0.338 (proposal 0183): the marked named slot is retired. Flipped from
    // the admitted run row; the parser now rejects `move` at a named slot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a slot spelling"));
    BEZEL_EXPECT_TRUE(diagContains(r, "positional destructure"));
}

// ===========================================================================
// LOOPED DISPOSAL ORACLE — the single-payload adoption, 200x.
//
// This row pins BOTH error directions at once:
//
//   * FAIL TO MARK a slot the arm adopted  -> the arm's `using var w` disposes
//     the payload AND the construct-completion reclaim disposes it again: a
//     DOUBLE FREE, observable here as 400 destructor lines.
//   * MARK a slot the arm did not adopt -> nobody reclaims it: a LEAK,
//     observable here as 0 destructor lines.
//
// So the assertion is an EXACT count of 200, looped. A one-shot version of
// this row would pass on both wrong implementations.
// ===========================================================================
BEZEL_TEST(named_slot_marked_owning_4763b,
           marked_named_slot_adoption_is_rejected_looped_v0_338) {
    auto r = runAot(loopedProgram(
        1,
        "        Fetched(move p0 = var r) => do "
        "          using var w := move r; "
        "        end, "
        "        Failed(_) => Console.WriteLine(\"failed\") "));
    // v0.338: flipped from the looped disposal row (200 destructor lines).
    // No program with a marked named slot now compiles, so there is no
    // disposal to count; the positional control rows keep the oracle.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a slot spelling"));
}

// ===========================================================================
// PER-SLOT COMPOSITION — §19.3: "Any number of slots in one list may be marked:
// the admission conditions and the transfer semantics are per-slot and compose
// … and unmarked named slots in the same list keep the borrow reading above."
//
// Two `owned` payloads, ONE slot marked. The marked one transfers and is
// adopted by the arm; the unmarked one stays a borrow and must be reclaimed by
// §35.5's construct-completion pass ("the un-transferred remainder — unlisted
// fields, unmarked named slots, discarded siblings — is disposed at construct
// completion").
//
// 400 destructor lines over 200 iterations IS the whole rule in one number: 200
// adopted + 200 completion-reclaimed. A codegen skip widened to "all named
// slots" gives 200 (the unmarked sibling LEAKS); narrowed to "no named slot"
// gives 600 (the adopted one is disposed twice).
// ===========================================================================
BEZEL_TEST(named_slot_marked_owning_4763b,
           marked_and_unmarked_named_slots_compose_per_slot_is_rejected_v0_338) {
    auto r = runAot(loopedProgram(
        2,
        "        Fetched(move p0 = var r, p1 = var b) => do "
        "          using var w := move r; "
        "        end, "
        "        Failed(_) => Console.WriteLine(\"failed\") "));
    // v0.338: flipped from the looped composition row (400 destructor lines).
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a slot spelling"));
}

// DIFFERENTIAL CONTROL for the row above — the POSITIONAL spelling of the same
// two-payload case. §35.5 transfers "the case's `owned` and move-only value
// payloads into its bindings", so BOTH positional bindings are owned locals of
// the arm scope and BOTH must be discharged for the program to be leak-free.
//
// The two rows together are the differential that matters: the named spelling
// discharges ONE binding and still reclaims 400, the positional spelling
// discharges TWO and reclaims 400. Identical totals, different composition —
// which is exactly "multiple named transfers spell a subset of what the
// construct already does".
//
// NOTE on what this control does NOT assert: the positional spelling with only
// `p0` discharged LEAKS `p1` today (MEASURED: 200 lines). That is the
// pre-existing must-use divergence corpus row s35_5_0031 records, independent
// of this rule; pinning "it leaks" here would freeze a divergence into a green
// row, so this control discharges both bindings instead.
BEZEL_TEST(named_slot_marked_owning_4763b,
           positional_two_slots_both_discharged_differential_control) {
    auto r = runAot(loopedProgram(
        2,
        "        Fetched(var r, var b) => do "
        "          using var w := move r; "
        "          using var w2 := move b; "
        "        end, "
        "        Failed(_) => Console.WriteLine(\"failed\") "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outContains(r, "done"));
    BEZEL_EXPECT_EQ(countOut(r, "widget disposed"), 2 * kIters);
}

// DIFFERENTIAL CONTROL — the POSITIONAL spelling of the single-slot adoption
// row. A whole-slot binding of an `owned` payload in an owning match IS
// transferred into its binding and `move r` on it is admitted. If this goes red
// the change over-reached and closed the positional transfer.
BEZEL_TEST(named_slot_marked_owning_4763b,
           positional_single_slot_adoption_differential_control) {
    auto r = runAot(loopedProgram(
        1,
        "        Fetched(var r) => do "
        "          using var w := move r; "
        "        end, "
        "        Failed(_) => Console.WriteLine(\"failed\") "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outContains(r, "done"));
    BEZEL_EXPECT_EQ(countOut(r, "widget disposed"), kIters);
}

// §15.3's half of the rule — a MARKED named slot over a payload of MOVE-ONLY
// VALUE TYPE. §15.3: "a case-payload of move-only value type matched by a
// **positional slot or a marked named slot** instead **transfers into its
// binding at arm selection**". A nested owning choice is the move-only payload
// with a pattern surface; it reaches a different rejection map in ownership
// (`borrowOnlyMoveOnlyArmBindings_`), so it is worth its own row.
//
// The positional spelling of the same program is the control, and the two rows
// assert the SAME disposal count (MEASURED: 200 each).
namespace {
// `slotText` is the WHOLE slot as written: the named spellings
// `move inner = var h` and `inner = var h`, or the positional spelling
// `var h` — so the two differ by exactly the marker and nothing else.
std::string nestedMoveOnlyProgram(const char* slotText) {
    return "module Conf; "
           "public class W4763b "
           "  public constructor() end constructor; "
           "  destructor Console.WriteLine(\"w disposed\"); "
           "    end destructor; "
           "end W4763b; "
           "public choice Slot4763b Empty4763b, Full4763b(owned held: W4763b); "
           "end Slot4763b; "
           "public choice Outer4763b None4763b, Has4763b(owned inner: Slot4763b); "
           "end Outer4763b; "
           "public class Program "
           "  public static method Main() "
           "    for var i in 0..<200 do "
           "      case Outer4763b.Has4763b(Slot4763b.Full4763b(new W4763b())) of "
           "        Has4763b(" + std::string(slotText) + ") => do "
           "          using var w := move h; "
           "        end, "
           "        None4763b => () "
           "      end case; "
           "    end; "
           "    Console.WriteLine(\"done\"); "
           "  end Main; "
           "end Program; ";
}
}  // namespace

BEZEL_TEST(named_slot_marked_owning_4763b,
           marked_named_slot_over_move_only_payload_is_rejected_looped_v0_338) {
    auto r = runAot(nestedMoveOnlyProgram("move inner = var h"));
    // v0.338: flipped from the admitted run row (200 "w disposed" lines).
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a slot spelling"));
}

BEZEL_TEST(named_slot_marked_owning_4763b,
           positional_move_only_slot_differential_control) {
    auto r = runAot(nestedMoveOnlyProgram("var h"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outContains(r, "done"));
    BEZEL_EXPECT_EQ(countOut(r, "w disposed"), 200);
}

// ===========================================================================
// THE DIRECTION THAT MUST NOT REGRESS — conformance case s35_5_0010's program,
// verbatim. An UNMARKED named slot still never transfers, so `move r` on its
// binding is still the §19.3 error it was.
//
// This is the row a careless widening of the codegen/ownership skip breaks
// first: admitting the marker must not have been done by admitting ALL named
// slots. The needle is the row's own authored `diagnostic:` — §19.3's sentence,
// "An unmarked named slot never transfers".
// ===========================================================================
BEZEL_TEST(named_slot_marked_owning_4763b,
           unmarked_named_slot_still_cannot_be_moved_spec_19_3_35_5) {
    auto r = runAot(owningChoice(1) +
        "public class Program "
        "  public static method Main() "
        "    using var client := new Client(); "
        "    case client.Fetch() of "
        "      Fetched(p0 = var r) => do "
        "        using var w := move r; "
        "      end, "
        "      Failed(_) => Console.WriteLine(\"failed\") "
        "    end case; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "never transfers"));
}

// ===========================================================================
// §19.3 condition (i) — the BORROWING-match refusal, over an owned LOCAL in
// scrutinee position. §35.5's borrowing-match default: "An **unmarked read of
// a named owned binding in scrutinee position stays a borrowing match** … it
// is what makes *peeking* at an owned outcome legal and non-consuming."
//
// This is the condition sema CANNOT decide (`owningMatchScrutineeForm` is the
// one authority, and it is in ownership), which is why the diagnostic is
// reported from the ownership pass rather than the normalization that admits
// the marker. The needle is the corpus row's authored `diagnostic:`
// (`borrowing match`); MEASURED on the pre-fix compiler that needle MISSED,
// because sema refused the marker on the bare spelling instead.
// ===========================================================================
BEZEL_TEST(named_slot_marked_owning_4763b,
           marked_named_slot_over_owned_local_scrutinee_rejected_spec_19_3) {
    auto r = runAot(owningChoice(1) +
        "public class Program "
        "  public static method Main() "
        "    using var client := new Client(); "
        "    var outcome := client.Fetch(); "
        "    case outcome of "
        "      Fetched(move p0 = var r) => Console.WriteLine(\"peeked\"), "
        "      Failed(_) => Console.WriteLine(\"failed\") "
        "    end case; "
        "    _ := outcome; "
        "  end Main; "
        "end Program; ");
    // v0.338: the marker is now a parse error, so the borrowing-match
    // diagnostic is no longer reached for a marked slot.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a slot spelling"));
    BEZEL_EXPECT_TRUE(diagContains(r, "positional destructure"));
}

// The same refusal over a PARAMETER scrutinee — conformance case s35_5_0009's
// program, verbatim modulo the field name. A second route to the same
// condition, and the row that carries the `borrowing match` needle in the
// corpus.
BEZEL_TEST(named_slot_marked_owning_4763b,
           marked_named_slot_over_parameter_scrutinee_rejected_spec_19_3) {
    auto r = runAot(owningChoice(1) +
        "public class Program "
        "  public static method Peek(outcome: Outcome) "
        "    case outcome of "
        "      Fetched(move p0 = var r) => Console.WriteLine(\"ok\"), "
        "      Failed(_) => Console.WriteLine(\"failed\") "
        "    end case; "
        "    _ := outcome; "
        "  end Peek; "
        "  public static method Main() "
        "    Console.WriteLine(1); "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a slot spelling"));
}

// ===========================================================================
// §19.3 condition (ii), first refusal — "on a **plain (unmarked) class- or
// interface-typed payload field** of an owning case (the slot is a borrow, not
// an obligation — the case's owning classification comes from a sibling field,
// §35.4, and an unmarked field routes nothing to transfer; the remedy is to
// mark the field `owned` at the declaration if the case is meant to own it)".
//
// The needle is that remedy: a diagnostic that only refused the program would
// leave the reader with no way to make it legal.
// ===========================================================================
BEZEL_TEST(named_slot_marked_owning_4763b,
           marker_on_plain_class_payload_field_rejected_spec_19_3) {
    auto r = runAot(
        "module Conf; "
        "public class Widget "
        "  public constructor() end constructor; "
        "end Widget; "
        "public choice Outcome "
        "  Fetched(owned response: Widget), "
        "  Broken(spare: Widget, status: int), "
        "  Failed(status: int); "
        "end Outcome; "
        "public class Program "
        "  public static method Main() "
        "    case Outcome.Broken(new Widget(), 1) of "
        "      Broken(move spare = var s, status = var k) => "
        "        Console.WriteLine(\"x\"), "
        "      _ => Console.WriteLine(\"y\") "
        "    end case; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a slot spelling"));
    BEZEL_EXPECT_TRUE(diagContains(r, "positional destructure"));
}

// §19.3 condition (ii), second refusal — "on a **copyable value, `string`, or
// runtime-managed payload** (no obligation to transfer …)".
//
// This row is the one a marker-only test for (ii) gets WRONG in the accepting
// direction: `owned note: string` IS `owned`-MARKED, so a `modifier == "owned"`
// test admits it and the program compiles (MEASURED on that tree). §19.3's own
// wording for (ii) is "`owned`-marked", and §28.1's oracle is what tells a
// marker that MEANS something from one it ERASES on a runtime-managed type —
// so the test is that oracle, not the modifier alone.
BEZEL_TEST(named_slot_marked_owning_4763b,
           marker_on_string_payload_rejected_spec_19_3_28_1) {
    auto r = runAot(
        "module Conf; "
        "public class Widget "
        "  public constructor() end constructor; "
        "end Widget; "
        "public choice Outcome "
        "  Fetched(owned response: Widget), "
        "  Tagged(owned note: string, status: int), "
        "  Failed(status: int); "
        "end Outcome; "
        "public class Program "
        "  public static method Main() "
        "    case Outcome.Tagged(\"hello\", 1) of "
        "      Tagged(move note = var s, status = var k) => "
        "        Console.WriteLine(s), "
        "      _ => Console.WriteLine(\"y\") "
        "    end case; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a slot spelling"));
    BEZEL_EXPECT_TRUE(diagContains(r, "positional destructure"));
}

// ===========================================================================
// §19.3 condition (iii) — "the slot's pattern is a top-level binding (`var x`;
// v1)", and its own sentence for the discard: "A **`_` right-hand side**
// (`move N = _`) is already inadmissible under condition (iii) — `_` is no
// binding — and its diagnostic names the sanctioned forms and the remedy per
// §17.13's discipline: *"a discarded payload is already disposed at construct
// completion — drop the marker, or bind with `var x` to transfer."*
// ===========================================================================
BEZEL_TEST(named_slot_marked_owning_4763b,
           marker_on_discard_right_hand_side_rejected_spec_19_3) {
    auto r = runAot(owningChoice(1) +
        "public class Program "
        "  public static method Main() "
        "    using var client := new Client(); "
        "    case client.Fetch() of "
        "      Fetched(move p0 = _) => Console.WriteLine(\"dropped\"), "
        "      Failed(_) => Console.WriteLine(\"failed\") "
        "    end case; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a slot spelling"));
    BEZEL_EXPECT_TRUE(diagContains(r, "positional destructure"));
}

// The head condition — "on a **record or class** pattern head — by derivation,
// not fiat: no obligation-bearing slot can exist there (records carry no
// `owned` fields, §28.5, and a class getter may not mint an owned value,
// §28.6), so marked slots are choice-case-only".
//
// The row also pins that this produces ONE diagnostic, not two: a rejected
// marker must not survive normalization into the ownership pass's
// condition-(i) refusal (MEASURED on the tree that left it set: the record-head
// program drew BOTH "admitted only on a choice case head" and "this scrutinee
// leaves a borrowing match", the second naming a condition that was never the
// failing one).
BEZEL_TEST(named_slot_marked_owning_4763b,
           marker_on_record_head_rejected_once_spec_19_3_28_5_28_6) {
    auto r = runAot(
        "module Conf; "
        "public record Rect4763b(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var rect := new Rect4763b(3, 4); "
        "    case rect of "
        "      Rect4763b(Width = 3, move Height = var h) => "
        "        Console.WriteLine(h.ToString()), "
        "      _ => Console.WriteLine(\"other\") "
        "    end case; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not a slot spelling"));
    BEZEL_EXPECT_TRUE(diagContains(r, "positional destructure"));
    // Exactly ONE diagnostic: the head condition, not also the borrowing match.
    int errors = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) ++errors;
    BEZEL_EXPECT_EQ(errors, 1);
}

// A named slot over a RECORD head is otherwise untouched — this is
// conformance row s19_3_0007's own program, and the differential control for
// the row above: the head condition refuses the MARKED spelling without
// touching the UNMARKED one.
BEZEL_TEST(named_slot_marked_owning_4763b,
           record_named_slot_unaffected_control_spec_19_3) {
    auto r = runAot(
        "module Conf; "
        "public record Rect4763b(Width: int, Height: int); "
        "public class Program "
        "  public static method Main() "
        "    var rect := new Rect4763b(3, 4); "
        "    case rect of "
        "      Rect4763b(Width = 3, Height = var h) => "
        "        Console.WriteLine(h.ToString()), "
        "      _ => Console.WriteLine(\"other\") "
        "    end case; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(outContains(r, "4"));
}