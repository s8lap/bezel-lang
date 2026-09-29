// fix #3774 / §22.13 (Static initialization order) / §23.1 (Auto-properties),
// v0.286: a `static readonly` AUTO-PROPERTY whose initializer reads a
// later-declared static-readonly sibling was neither rejected at compile time
// nor caught by the runtime partial-state guard — it silently stored the wrong
// value into its backing global, permanently.
//
// The two spec sentences that make this one rule, quoted verbatim:
//
//   §22.13: "Within a single type, static fields are initialized in textual
//   declaration order." … "Forward references between `static readonly` fields
//   of the same type are a compile error. Specifically: if the initializer
//   expression for a `static readonly` field F reads a `static readonly` field
//   G of the same type declared *after* F in the source … the reference is
//   rejected at compile time."
//
//   §23.1: a `readonly` auto-property's assignment "is a store to the backing
//   field (no setter exists to invoke, §22.7 rule 1), and it is admitted only
//   through `this` for an instance auto-property, or in the declaring type's
//   own static initialization for a `static readonly` one."
//
// So a static-readonly auto-property's §23.1 backing field is one of the very
// slots §22.13 orders "in textual declaration order", and the forward-reference
// rule ranges over it on BOTH sides of a reference. Before this fix:
//
//   * `ExprTypecheck.cpp`'s `checkStaticReadonlyForwardRef` ran only for a
//     `static readonly` FieldDecl SOURCE, and its `laterSR` lambda recognized
//     only a `FieldSymbol` TARGET — so property→property, field→property and
//     property→field all sailed through.
//   * `Emitter::emitStaticPartialReadGuard`'s two call sites in
//     `LlvmBackend_Exprs.cpp` were gated on `dynamic_cast<const
//     sema::FieldSymbol*>(sym)`, and `emitStaticConstructorBody`'s
//     PropertySymbol arm never called `bumpProgress` at all — so the runtime
//     backstop could not fire on a property either as reader or as target.
//
// Issue's own measured repro (HEAD 63fa583, post-#3749): `P1 := C.P2 + 1;`
// above `P2 := H.Get();` compiled clean and printed `P1=1 P2=77` — P1 observed
// P2's zero default plus one. #3749 (which made a static auto-property's
// non-foldable initializer actually RUN for the first time) is what turned the
// old uniform zero into a plausible-looking wrong value.
//
// Rows:
//   t1  property → LATER property   — REJECTED (§22.13). Issue's own repro.
//   t1b same, error text cites §22.13 and names both members.
//   t2  field    → LATER property   — REJECTED (the issue's second, pre-#3749
//                                     shape, `F=1` before and after that fix).
//   t3  property → LATER field      — REJECTED (was a *runtime* TIE only).
//   t4  CONTROL: property → EARLIER property — accepted, correct value.
//   t5  CONTROL: field → LATER field — still rejected (the pre-existing rule,
//                                     unchanged by this fix).
//   t6  CONTROL: INSTANCE auto-properties — untouched (§22.13 is static-only).
//   t7  a property→later-property reference routed through a static helper's
//                                     BODY — REJECTED. (Written at v0.286
//                                     asserting a runtime TIE; FLIPPED to
//                                     compile-time rejection by fix #4531,
//                                     which implements §22.13's "transitively
//                                     through the bodies of
//                                     statically-resolvable calls" clause. The
//                                     row's own comment quotes the sentences.)
//   t8  CONTROL: the same helper routing in CORRECT declaration order — no
//                                     raise, correct value.
//   t9  CONTROL (over-reject guard): a later static FIELD reading an EARLIER
//                                     static auto-property, both non-foldable —
//                                     the widened slot index/guard must not
//                                     turn this legal backward read into a
//                                     rejection or a spurious raise.
//   t10 HOST-KIND: `record` host — rejected.
//   t11 HOST-KIND: `static class` host — rejected.
//   t12 HOST-KIND: GENERIC host — rejected on the template.
//   t13 CONTROL (exemption): §22.13's `const` carve-out — a static-readonly
//                                     auto-property reading a LATER `const` is
//                                     accepted and folds.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff some diagnostic is the §22.13 forward-reference rejection.
bool hasForwardRefError(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("declared later in the same type") !=
                std::string::npos &&
            d.message.find("\xc2\xa7" "22.13") != std::string::npos)
            return true;
    return false;
}

} // namespace

// t1 — issue #3774's own measured repro: a `static readonly` auto-property
// whose initializer reads a LATER-declared `static readonly` auto-property.
// Before the fix this compiled clean and printed `P1=1 P2=77`.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t1_static_readonly_property_reads_later_property_rejected_22_13) {
    auto r = runAot(
        "module M; "
        "public class H3774T1 "
        "  public static method Get(): int => 77; "
        "end H3774T1; "
        "public class C3774T1 "
        "  public static readonly property P1: int := C3774T1.P2 + 1; "
        "  public static readonly property P2: int := H3774T1.Get(); "
        "end C3774T1; "
        "public class Main3774T1 "
        "  public static method Main(): int "
        "    Console.WriteLine($\"P1={C3774T1.P1} P2={C3774T1.P2}\"); "
        "    return 0; "
        "  end Main; "
        "end Main3774T1;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasForwardRefError(r));
}

// t1b — the diagnostic must NAME BOTH members, per §22.13's "The error message
// names both fields and points to the textual ordering rule." Asserting on
// "did not compile" alone would also be satisfied by an unrelated rejection.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t1b_forward_ref_diagnostic_names_both_members_22_13) {
    auto r = runAot(
        "module M; "
        "public class H3774T1b "
        "  public static method Get(): int => 77; "
        "end H3774T1b; "
        "public class C3774T1b "
        "  public static readonly property Alpha: int := C3774T1b.Beta + 1; "
        "  public static readonly property Beta: int := H3774T1b.Get(); "
        "end C3774T1b; "
        "public class Main3774T1b "
        "  public static method Main(): int "
        "    return C3774T1b.Alpha; "
        "  end Main; "
        "end Main3774T1b;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool named = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("`Alpha`") != std::string::npos &&
            d.message.find("`Beta`") != std::string::npos)
            named = true;
    BEZEL_EXPECT_TRUE(named);
}

// t2 — the issue's SECOND shape: a `static readonly` FIELD whose initializer
// reads a LATER-declared `static readonly` auto-property. Pre-existing (the
// issue confirmed `F=1` both before and after #3749) — the field-source arm ran,
// but `laterSR` did not recognize a PropertySymbol target.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t2_static_readonly_field_reads_later_property_rejected_22_13) {
    auto r = runAot(
        "module M; "
        "public class H3774T2 "
        "  public static method Get(): int => 77; "
        "end H3774T2; "
        "public class C3774T2 "
        "  public static readonly F: int := C3774T2.P + 1; "
        "  public static readonly property P: int := H3774T2.Get(); "
        "end C3774T2; "
        "public class Main3774T2 "
        "  public static method Main(): int "
        "    Console.WriteLine($\"F={C3774T2.F} P={C3774T2.P}\"); "
        "    return 0; "
        "  end Main; "
        "end Main3774T2;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasForwardRefError(r));
}

// t3 — the third direction: a `static readonly` auto-property reading a
// LATER-declared `static readonly` FIELD. Before the fix the *runtime* guard
// caught this one (the target is a FieldSymbol, so the read-site guard fired)
// and it died with a TypeInitializationException at run time. §22.13 rejects it
// at COMPILE time, which is strictly earlier and names both members.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t3_static_readonly_property_reads_later_field_rejected_22_13) {
    auto r = runAot(
        "module M; "
        "public class H3774T3 "
        "  public static method Get(): int => 77; "
        "end H3774T3; "
        "public class C3774T3 "
        "  public static readonly property P: int := C3774T3.F + 1; "
        "  public static readonly F: int := H3774T3.Get(); "
        "end C3774T3; "
        "public class Main3774T3 "
        "  public static method Main(): int "
        "    Console.WriteLine($\"P={C3774T3.P} F={C3774T3.F}\"); "
        "    return 0; "
        "  end Main; "
        "end Main3774T3;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasForwardRefError(r));
}

// t4 — CONTROL: the same two auto-properties in the CORRECT textual order. A
// BACKWARD read is exactly what §22.13's declaration-order rule licenses, so
// this must compile and produce 78 (77 + 1). Guards against a fix that rejects
// every property-to-property reference.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t4_control_property_reads_earlier_property_accepted_22_13) {
    auto r = runAot(
        "module M; "
        "public class H3774T4 "
        "  public static method Get(): int => 77; "
        "end H3774T4; "
        "public class C3774T4 "
        "  public static readonly property P2: int := H3774T4.Get(); "
        "  public static readonly property P1: int := C3774T4.P2 + 1; "
        "end C3774T4; "
        "public class Main3774T4 "
        "  public static method Main(): int "
        "    Console.WriteLine($\"P1={C3774T4.P1} P2={C3774T4.P2}\"); "
        "    return 0; "
        "  end Main; "
        "end Main3774T4;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "P1=78 P2=77\n");
}

// t5 — CONTROL: the pre-existing field→later-field rejection (#747 R42) is
// unchanged by this fix. The source arm was refactored to share a core with the
// property arm; this row pins that the field spelling still rejects.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t5_control_field_reads_later_field_still_rejected_22_13) {
    auto r = runAot(
        "module M; "
        "public class C3774T5 "
        "  public static readonly A: int := C3774T5.B + 1; "
        "  public static readonly B: int := 42; "
        "end C3774T5; "
        "public class Main3774T5 "
        "  public static method Main(): int "
        "    return C3774T5.A; "
        "  end Main; "
        "end Main3774T5;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasForwardRefError(r));
}

// t6 — CONTROL: INSTANCE auto-properties are outside §22.13 entirely (it
// governs *static* initialization order; §23.1's instance half is "through
// `this`", under §22.8's per-instance regime). Neither declaration order nor
// the widened check may touch them.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t6_control_instance_auto_properties_unaffected_23_1) {
    auto r = runAot(
        "module M; "
        "public class C3774T6 "
        "  public readonly property A: int := 1; "
        "  public readonly property B: int := 2; "
        "end C3774T6; "
        "public class Main3774T6 "
        "  public static method Main(): int "
        "    var c := new C3774T6(); "
        "    Console.WriteLine($\"A={c.A} B={c.B}\"); "
        "    return 0; "
        "  end Main; "
        "end Main3774T6;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "A=1 B=2\n");
}

// t7 — FLIPPED by fix #4531 (§22.13, v0.306): rejected AT COMPILE TIME, not
// left to a runtime raise.
//
// This row was written (v0.286) asserting a runtime TypeInitializationException,
// on the reading that a reference routed through a helper method's BODY is one
// §22.13's "deliberate approximation" leaves to rule 2's runtime backstop. That
// reading does not survive the section's own text, on either half:
//
//   * The compile-time rule already covers this route. §22.13: "if the
//     initializer expression for a `static readonly` field F reads a `static
//     readonly` field G of the same type declared *after* F in the source —
//     **directly, or transitively through the bodies of statically-resolvable
//     calls** (the same call set as the cross-type dependency graph below —
//     static methods and static property getters reached by direct name within
//     the compilation unit, §2.5; function values, virtual dispatch, and code
//     outside the compilation unit contribute nothing) — the reference is
//     rejected at compile time." `C3774T7.Helper()` IS a static method reached
//     by direct name within the compilation unit, so it is IN that call set,
//     and the escapes the approximation actually grants are named exhaustively
//     in the next sentence: "a same-type forward read routed through a function
//     value, virtual dispatch, or a module boundary compiles". A direct static
//     call is none of the three.
//
//   * Rule 2's backstop never governed this shape. It fires on "a touch of a
//     type whose static initialization is in progress on the same thread,
//     arriving from **outside that type's own initialization**" — "a touch of T
//     is *re-entrant* when that innermost initialization belongs to a different
//     type — the T → U → T signature — and is *ordinary execution* when it is
//     T's own". Here C3774T7's own initializer reads C3774T7's own later
//     static, so the touch is ordinary execution. The paragraph says so again
//     itself: "(A type's *own* initialization code observing its own later
//     statics is governed by the forward-reference rule and its recorded
//     residual above, not by this backstop.)"
//
// So the TIE this row asserted was the runtime raising where §22.13 does not
// ask it to. Fix #4486 stopped that raise — correctly, per the paragraph above
// — and the shape then reached `Main` and printed the silently-wrong
// `P1=1 P2=77` (measured at 87313e75), which is exactly the outcome #3774
// exists to eliminate. The defect the two fixes together expose is that the
// compile-time check implemented only the DIRECT half of its own rule. Fix
// #4531 implements the transitive half in StaticInitCycles.cpp — the pass that
// already owns §22.13's statically-resolvable call set — so the program is now
// refused at compile time, which is both what the spec says and a strictly
// stronger oracle than the runtime raise this row used to accept.
//
// The residual #4486 pins (static_same_type_forward_ref_residual_4486_test.cpp)
// is untouched: its rows route through a LAMBDA, one of the three named
// escapes, which this check deliberately does not walk.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t7_property_forward_ref_through_helper_body_rejected_22_13) {
    auto r = runAot(
        "module M; "
        "public class H3774T7 "
        "  public static method Get(): int => 77; "
        "end H3774T7; "
        "public class C3774T7 "
        "  public static readonly property P1: int := C3774T7.Helper(); "
        "  public static readonly property P2: int := H3774T7.Get(); "
        "  public static method Helper(): int => C3774T7.P2 + 1; "
        "end C3774T7; "
        "public class Main3774T7 "
        "  public static method Main(): int "
        "    Console.WriteLine($\"P1={C3774T7.P1} P2={C3774T7.P2}\"); "
        "    return 0; "
        "  end Main; "
        "end Main3774T7;");
    // Never runs at all: the program does not compile. In particular it never
    // reaches the silently-wrong "P1=1 P2=77" (P2's §7.9 `int` default of 0,
    // plus one) that #3774 exists to eliminate.
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasForwardRefError(r));
    // Names BOTH members, per §22.13's "The error message names both fields
    // and points to the textual ordering rule" — the same obligation t1b pins
    // for the direct arm. Without this the row would also be satisfied by an
    // unrelated rejection.
    bool named = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("`P1`") != std::string::npos &&
            d.message.find("`P2`") != std::string::npos)
            named = true;
    BEZEL_EXPECT_TRUE(named);
}

// t8 — CONTROL for t7: the same helper routing with the two properties in the
// CORRECT declaration order. `P2` is stored before `P1`'s initializer runs, so
// the backward read through the helper body is ordinary self-execution — the
// case §22.13 explicitly protects ("what lets a field initializer call the
// type's own static helpers"). Guards against a guard that raises on every
// same-type static read during initialization.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t8_control_helper_body_read_in_declaration_order_no_raise_22_13) {
    auto r = runAot(
        "module M; "
        "public class H3774T8 "
        "  public static method Get(): int => 77; "
        "end H3774T8; "
        "public class C3774T8 "
        "  public static readonly property P2: int := H3774T8.Get(); "
        "  public static readonly property P1: int := C3774T8.Helper(); "
        "  public static method Helper(): int => C3774T8.P2 + 1; "
        "end C3774T8; "
        "public class Main3774T8 "
        "  public static method Main(): int "
        "    Console.WriteLine($\"P1={C3774T8.P1} P2={C3774T8.P2}\"); "
        "    return 0; "
        "  end Main; "
        "end Main3774T8;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "P1=78 P2=77\n");
}

// t9 — CONTROL (over-reject guard for the widened slot index). Interleaving a
// static auto-property between two static fields changes every later field's
// slot ordinal, because `staticInitSlotIndex` now counts property slots too. A
// LATER field reading BOTH earlier slots must still see their real values, and
// must neither be rejected nor raise: `F2 = F1 + P = 10 + 20 = 30`.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t9_control_later_field_reads_earlier_field_and_property_22_13) {
    auto r = runAot(
        "module M; "
        "public class H3774T9 "
        "  public static method A(): int => 10; "
        "  public static method B(): int => 20; "
        "end H3774T9; "
        "public class C3774T9 "
        "  public static readonly F1: int := H3774T9.A(); "
        "  public static readonly property P: int := H3774T9.B(); "
        "  public static readonly F2: int := C3774T9.F1 + C3774T9.P; "
        "end C3774T9; "
        "public class Main3774T9 "
        "  public static method Main(): int "
        "    Console.WriteLine($\"F1={C3774T9.F1} P={C3774T9.P} "
        "F2={C3774T9.F2}\"); "
        "    return 0; "
        "  end Main; "
        "end Main3774T9;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "F1=10 P=20 F2=30\n");
}

// t10 — HOST-KIND axis: `record`. §22.13's rule is a property of static
// initialization, not of the class keyword, and #3749's own suite measured the
// record host identically to the class host.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t10_record_host_property_forward_ref_rejected_22_13) {
    auto r = runAot(
        "module M; "
        "public class H3774T10 "
        "  public static method Get(): int => 7; "
        "end H3774T10; "
        "public record R3774T10 "
        "  public static readonly property P1: int := R3774T10.P2 + 1; "
        "  public static readonly property P2: int := H3774T10.Get(); "
        "end R3774T10; "
        "public class Main3774T10 "
        "  public static method Main(): int "
        "    return R3774T10.P1; "
        "  end Main; "
        "end Main3774T10;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasForwardRefError(r));
}

// t11 — HOST-KIND axis: `static class` (§11.7), whose whole member surface is
// static and which §22.13's disposal text names explicitly.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t11_static_class_host_property_forward_ref_rejected_22_13) {
    auto r = runAot(
        "module M; "
        "public class H3774T11 "
        "  public static method Get(): int => 7; "
        "end H3774T11; "
        "public static class S3774T11 "
        "  public static readonly property Q1: int := S3774T11.Q2 + 1; "
        "  public static readonly property Q2: int := H3774T11.Get(); "
        "end S3774T11; "
        "public class Main3774T11 "
        "  public static method Main(): int "
        "    return S3774T11.Q1; "
        "  end Main; "
        "end Main3774T11;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasForwardRefError(r));
}

// t12 — HOST-KIND axis: a GENERIC type. The static auto-property emitter's own
// note says per-instantiation carriage has no property analogue yet; the
// compile-time rejection is declaration-level and must still fire on the
// template, so a generic host cannot be used to smuggle the shape past.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t12_generic_host_property_forward_ref_rejected_22_13) {
    auto r = runAot(
        "module M; "
        "public class H3774T12 "
        "  public static method Get(): int => 7; "
        "end H3774T12; "
        "public class G3774T12<T> "
        "  public static readonly property P1: int := G3774T12<T>.P2 + 1; "
        "  public static readonly property P2: int := H3774T12.Get(); "
        "end G3774T12; "
        "public class Main3774T12 "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Main3774T12;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasForwardRefError(r));
}

// t13 — CONTROL (exemption): §22.13 — "Forward references between `const`
// declarations are not subject to this rule: a `const` initializer must be a
// compile-time-constant expression … so forward references between consts are
// either resolved at compile time (the textual order doesn't matter — consts
// have no temporal initialization) or are themselves a compile error for
// non-constant operands." A static-readonly auto-property reading a
// LATER-declared `const` must therefore be accepted, and fold: 5 + 6 = 11.
BEZEL_TEST(sema_static_autoprop_forward_ref_3774,
           t13_control_property_reads_later_const_accepted_22_13) {
    auto r = runAot(
        "module M; "
        "public class C3774T13 "
        "  public const K: int := 5; "
        "  public static readonly property P: int := C3774T13.K + C3774T13.J; "
        "  public const J: int := 6; "
        "end C3774T13; "
        "public class Main3774T13 "
        "  public static method Main(): int "
        "    Console.WriteLine($\"P={C3774T13.P}\"); "
        "    return 0; "
        "  end Main; "
        "end Main3774T13;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "P=11\n");
}
