// Spec-derived tests for fix #4014 — §22.7 (v0.294) rule 1's this-observation
// gate on a RECORD constructor.
//
// §22.7 ("Observing `this` during construction") states three rules that
// "keep §7.9's guarantee true while `this` is under construction":
//
//   Rule 1 (the observation gate): "a synchronous observation of `this` — an
//   instance method, block-form accessor, or indexer invocation with receiver
//   `this` (explicit or implicit); `this` as an argument; `this` stored
//   anywhere; ... — is admitted only at points where every field of the
//   declaring class that §7.9 requires to be explicitly initialized is
//   definitely assigned on every reaching path."
//
//   Rule 2: "No virtual dispatch from constructor bodies of non-sealed
//   classes." Its own text is explicit: "A call from a constructor body to a
//   ... member ... is a compile error unless the declaring class is `sealed`
//   and not `abstract`." This rule's hazard — a derived override running over
//   not-yet-initialized fields — genuinely requires inheritance to exist.
//
//   Rule 3: capturing `this` in a lambda is NOT itself an observation ("is
//   not gated by rule 1"); only an admitted closure's INVOCATION before
//   construction completes would be one (a residual this file does not
//   reach).
//
// Before this fix, `DefiniteAssignment.cpp`'s `runRecordConstructor`
// (fix #4001/#2207) tracked `fieldsNeedingInit_`/`assignedFields_` for a
// record exactly as `runConstructor` does for a class, but `recordCtorMode_`
// never reached `gateObservation`/`visitCtorCall`/`visitCtorMemberAccess` —
// those three functions, and the bare-`this`-identifier check in `visitExpr`,
// were gated on `ctorMode_` alone (set only by `runConstructor`, i.e. only for
// classes). The comment on that design explicitly (and, per this fix,
// wrongly) attributed the WHOLE observation gate to "records have no
// base-first-construction/virtual-dispatch footgun" — but that footgun is
// rule 2's alone; rule 1's hazard (an ordinary instance call/property read of
// a not-yet-assigned field through `this`) has nothing to do with
// inheritance, and reproduces identically on a record's own sealed,
// non-virtual methods. The practical consequence, exactly like #4001's own
// bug: `bezelc` accepted a record constructor that called `this.Method()` (or
// read a computed `this.Property`, or passed `this` as a bare argument)
// BEFORE assigning a non-nullable field that call/read observed, and the
// generated code read the unset (zero-filled) field storage and SIGSEGVed —
// one statement earlier in the same constructor #4001 already covers.
//
// Because the pre-fix symptom is a SIGSEGV/crash rather than a clean
// diagnostic-free compile, every REJECT-arm test below uses
// `failedAtCompileTimeWithRealLocation` (aot_result_asserts.hpp) rather than
// a bare `!r.ran`, per this project's `check_ambiguous_ran_false.py` ratchet.
//
// FAIL-WITHOUT/PASS-WITH, precisely (correcting an overclaim in fix #4014's
// own commit body, which said "all 12 ... are verified to fail" without the
// fix — the true figure is 5 of 12). Reverting `DefiniteAssignment.cpp` to
// its pre-#4014 state fails exactly the 5 REJECT-arm tests below —
// `instance_method_call_before_field_assigned_rejected_spec_22_7`,
// `implicit_this_call_before_field_assigned_rejected_spec_22_7`,
// `computed_property_read_before_field_assigned_rejected_spec_22_7`,
// `this_passed_as_argument_before_field_assigned_rejected_spec_22_7`, and
// `virtual_record_method_before_assignment_rejected_by_rule1_not_rule2` — and
// `class_control_instance_method_call_before_field_assigned_rejected` still
// PASSES pre-fix too, being a class (unaffected by this fix either way). The
// other 7 are ACCEPT-arm/control rows that correctly pass on BOTH sides of
// the fix BY DESIGN, not because the fix reaches them:
// `instance_method_call_after_field_assigned_accepted_spec_22_7` and
// `override_record_method_after_assignment_accepted` (field already assigned
// — the gate was never going to fire either way);
// `lambda_capture_of_this_before_field_assigned_accepted_spec_22_7` (rule 3:
// capture is never gated by rule 1, pre- or post-fix);
// `field_initializer_satisfies_gate_at_entry_accepted_spec_22_7` (the field
// carries an initializer, so it was never in `fieldsNeedingInit_` regardless
// of `recordCtorMode_` reaching `gateObservation`);
// `nullable_field_method_call_before_assignment_accepted_spec_7_9` (a
// nullable field is never §7.9-strict, so it was never in
// `fieldsNeedingInit_` either); and
// `this_chaining_ctor_body_call_accepted_spec_22_7` (the chained-to
// constructor's own §7.9 obligation already seeds every §7.9-strict field as
// assigned at body entry, pre-#4001 and unchanged by #4014). Each of these 7
// is an over-rejection GUARD per §15 of the lane rules, not evidence the fix
// does anything — their value is proving the fix does NOT newly reject code
// that should keep compiling, which the REJECT-arm count alone cannot show.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// Shared helper class every repro below funnels a non-nullable reference
// field through (mirrors kWidget in record_field_definite_assignment_4001_test.cpp).
const char* kWidget4014 =
    "module M; "
    "public class Widget4014 "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget4014; ";

}  // namespace

// =====================================================================
// REJECT arm — the issue's own repro and the adjacent observation forms
// §22.7 rule 1 names ("an instance method ... invocation"; "a block-form
// accessor ... invocation"; "`this` as an argument").
// =====================================================================

// The issue's exact repro: an ordinary (non-virtual) instance method call
// `this.Peek()` reads a not-yet-assigned non-nullable field through `Peek`'s
// body — no inheritance or dispatch involved anywhere in this program.
BEZEL_TEST(sema_record_ctor_observation_gate_4014,
           instance_method_call_before_field_assigned_rejected_spec_22_7) {
    auto r = runAot(std::string(kWidget4014) +
        "public record RecordObsCtor4014 "
        "  public readonly property Extra: Widget4014; "
        "  public constructor(w: Widget4014) "
        "    var x := this.Peek(); "
        "    Extra := w; "
        "  end constructor; "
        "  public method Peek(): int return Extra.Render(); end Peek; "
        "end RecordObsCtor4014; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget4014(42); "
        "    var c := new RecordObsCtor4014(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot observe `this`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "instance call `Peek`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.7"));
}

// A bare (implicit-`this`) call to a non-static instance method is the same
// observation under a different spelling (§22.7 rule 1: "receiver `this`
// (explicit or implicit)").
BEZEL_TEST(sema_record_ctor_observation_gate_4014,
           implicit_this_call_before_field_assigned_rejected_spec_22_7) {
    auto r = runAot(std::string(kWidget4014) +
        "public record RecordObsImplicit4014 "
        "  public readonly property Extra: Widget4014; "
        "  public constructor(w: Widget4014) "
        "    var x := Peek(); "
        "    Extra := w; "
        "  end constructor; "
        "  public method Peek(): int return Extra.Render(); end Peek; "
        "end RecordObsImplicit4014; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget4014(42); "
        "    var c := new RecordObsImplicit4014(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot observe `this`"));
}

// A computed (block-form) property getter invocation is a "block-form
// accessor ... invocation" observation (§22.7 rule 1) — distinct from a
// direct auto-property read, which the rule explicitly excludes.
BEZEL_TEST(sema_record_ctor_observation_gate_4014,
           computed_property_read_before_field_assigned_rejected_spec_22_7) {
    auto r = runAot(std::string(kWidget4014) +
        "public record RecordObsProp4014 "
        "  public readonly property Extra: Widget4014; "
        "  public property Peek: int "
        "    get => Extra.Render(); "
        "  end Peek; "
        "  public constructor(w: Widget4014) "
        "    var x := this.Peek; "
        "    Extra := w; "
        "  end constructor; "
        "end RecordObsProp4014; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget4014(42); "
        "    var c := new RecordObsProp4014(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot observe `this`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "property `Peek`"));
}

// `this` passed as a bare argument before a field it (transitively) exposes
// is assigned — §22.7 rule 1's "`this` as an argument" form. MEASURED: on the
// pre-fix compiler this specific shape does not SIGSEGV — it reaches a
// SEPARATE, pre-existing codegen defect (malformed IR, filed as its own
// issue) that fires whenever `this` is passed by value from inside a
// record's own constructor, regardless of field-assignment order. Rejecting
// here at sema time is strictly better than ever reaching that crash.
BEZEL_TEST(sema_record_ctor_observation_gate_4014,
           this_passed_as_argument_before_field_assigned_rejected_spec_22_7) {
    auto r = runAot(std::string(kWidget4014) +
        "public record RecordObsArg4014 "
        "  public readonly property Extra: Widget4014; "
        "  public static method Touch(x: RecordObsArg4014): int "
        "    return x.Extra.Render(); "
        "  end Touch; "
        "  public constructor(w: Widget4014) "
        "    var y := RecordObsArg4014.Touch(this); "
        "    Extra := w; "
        "  end constructor; "
        "end RecordObsArg4014; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget4014(42); "
        "    var c := new RecordObsArg4014(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot observe `this`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "reference to `this`"));
}

// =====================================================================
// ACCEPT arm (over-rejection guards) — each reads back a runtime VALUE, not
// merely a clean compile, so a false reject shows as a value/-999 mismatch.
// =====================================================================

// The identical instance-call shape, but the field is assigned FIRST — the
// gate's own admitted case. 42 round-trips.
BEZEL_TEST(sema_record_ctor_observation_gate_4014,
           instance_method_call_after_field_assigned_accepted_spec_22_7) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4014) +
        "public record RecordObsOk4014 "
        "  public readonly property Extra: Widget4014; "
        "  public constructor(w: Widget4014) "
        "    Extra := w; "
        "    var x := this.Peek(); "
        "  end constructor; "
        "  public method Peek(): int return Extra.Render(); end Peek; "
        "end RecordObsOk4014; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget4014(42); "
        "    var c := new RecordObsOk4014(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;"),
        42);
}

// §22.7 rule 3: capturing `this` in a lambda is NOT itself a synchronous
// observation ("is not gated by rule 1"). The lambda is bound but never
// invoked before the field is assigned — must be ACCEPTED even though the
// field is still unassigned at the capture site. 42 round-trips (the lambda
// is invoked only after construction completes, in `Main`).
BEZEL_TEST(sema_record_ctor_observation_gate_4014,
           lambda_capture_of_this_before_field_assigned_accepted_spec_22_7) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4014) +
        "public record RecordObsLambda4014 "
        "  public readonly property Extra: Widget4014; "
        "  public readonly property Deferred: () -> int; "
        "  public method Peek(): int return Extra.Render(); end Peek; "
        "  public constructor(w: Widget4014) "
        "    Deferred := lambda(): int => this.Peek(); "
        "    Extra := w; "
        "  end constructor; "
        "end RecordObsLambda4014; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget4014(42); "
        "    var c := new RecordObsLambda4014(w); "
        "    var f := c.Deferred; "
        "    return f(); "
        "  end Main; "
        "end C;"),
        42);
}

// A field with a field initializer counts as assigned at body entry (§22.7
// rule 1: "Fields with initializers count as assigned at body entry") — an
// instance call observing it must be ACCEPTED.
BEZEL_TEST(sema_record_ctor_observation_gate_4014,
           field_initializer_satisfies_gate_at_entry_accepted_spec_22_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record RecordObsInit4014 "
        "  public Extra: string := \"hello\"; "
        "  public method Peek(): int return Extra.Length; end Peek; "
        "  public constructor() "
        "    var x := this.Peek(); "
        "  end constructor; "
        "end RecordObsInit4014; "
        "class C "
        "  public static method Main(): int "
        "    var c := new RecordObsInit4014(); "
        "    return c.Peek(); "
        "  end Main; "
        "end C;"),
        5);
}

// A NULLABLE field is not §7.9-strict, so it never enters `fieldsNeedingInit_`
// and the gate has nothing to observe — an instance call before "assigning"
// it must be ACCEPTED regardless of assignment order (mirrors the existing
// `readonly_record_single_assignment_2207_test.cpp` control, which relies on
// exactly this to keep its own `this_escape_stored_lambda_rejected` case
// rejecting via the readonly once-check alone rather than this gate).
BEZEL_TEST(sema_record_ctor_observation_gate_4014,
           nullable_field_method_call_before_assignment_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record RecordObsNullable4014 "
        "  public Extra: string?; "
        "  public method Peek(): int "
        "    var e := Extra; "
        "    if e <> null then return e.Length; end; "
        "    return -1; "
        "  end Peek; "
        "  public constructor() "
        "    var x := this.Peek(); "
        "    Extra := \"hello\"; "
        "  end constructor; "
        "end RecordObsNullable4014; "
        "class C "
        "  public static method Main(): int "
        "    var c := new RecordObsNullable4014(); "
        "    return c.Peek(); "
        "  end Main; "
        "end C;"),
        5);
}

// §22.7 rule 2 stays genuinely class-only: a record method may carry the
// syntactically-admitted (but semantically inert — records are implicitly
// sealed, §9.6) `virtual` modifier, and calling it BEFORE the field is
// assigned is still rejected — but by RULE 1's observation-gate message, not
// rule 2's virtual-dispatch message, pinning that `checkVirtualDispatch`
// stays a no-op for records (its own `!ctorMode_` guard) rather than
// misfiring because `classSealedNonAbstract_` defaults to false outside
// `runConstructor`.
BEZEL_TEST(sema_record_ctor_observation_gate_4014,
           virtual_record_method_before_assignment_rejected_by_rule1_not_rule2) {
    auto r = runAot(std::string(kWidget4014) +
        "public record RecordObsVirtual4014 "
        "  public readonly property Extra: Widget4014; "
        "  public virtual method Peek(): int return Extra.Render(); end Peek; "
        "  public constructor(w: Widget4014) "
        "    var x := this.Peek(); "
        "    Extra := w; "
        "  end constructor; "
        "end RecordObsVirtual4014; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget4014(42); "
        "    var c := new RecordObsVirtual4014(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot observe `this`"));
    BEZEL_EXPECT_FALSE(diagContains(r, "virtual`/`override`/`abstract`"));
}

// A DISPATCH-MODIFIER-BEARING record method, called AFTER the field is
// assigned, must be ACCEPTED and run — confirming rule 2 truly never fires
// for a record (not merely that rule 1 masks it in the reject case above).
//
// The modifier is `override` on `ToString`, not `virtual`. §9.2 (v0.307):
// "A record member may not be declared `virtual`, `abstract`, or
// `sealed override`; `override` is admitted only on `Equals` ...
// `GetHashCode`, and `ToString`" — "Records are implicitly sealed (§9.6)
// and have no virtual-dispatch surface of their own, so the dispatch
// modifiers carry no meaning on record members; record members are always
// dispatched statically ... The rejection is at the declaration". So the
// original spelling of this row — a `virtual` record method — is not
// writable at all any more, and the row cannot be repaired by keeping it.
//
// It is repaired by taking the one dispatch modifier §9.2 DOES admit on a
// record member, which preserves the control's actual content: a record
// member bearing a dispatch modifier, invoked on `this` from the
// constructor after the observed field is assigned, is admitted. Dropping
// the modifier instead would make this row a duplicate of
// `instance_method_call_after_field_assigned_accepted_spec_22_7` above —
// coverage in appearance only.
//
// MEASURED that the control is not vacuous (bezelc, this tree): moving
// `var s := this.ToString();` ABOVE `Extra := w;` in the program below is
// REJECTED — "cannot observe `this` (instance call `ToString`) before
// non-nullable field `Extra` is definitely assigned on every path ...
// (§22.7, v0.184)". So rule 1 does reach an `override`-bearing record
// member, and this row's acceptance is the after-assignment fact, not an
// artifact of the member being invisible to the gate. That REJECT half is
// pinned by `virtual_record_method_before_assignment_rejected_by_rule1_not_rule2`
// above, which is untouched.
//
// The `override ToString` declaration costs the record only its auto-
// `ToString` (§9.2: "An `override` of `ToString` alone suppresses only the
// auto-`ToString`; auto-`Equals`/auto-`GetHashCode`/`=` remain"), which
// this row never uses. The assertion and expected value are unchanged.
BEZEL_TEST(sema_record_ctor_observation_gate_4014,
           override_record_method_after_assignment_accepted) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4014) +
        "public record RecordObsOverrideOk4014 "
        "  public readonly property Extra: Widget4014; "
        "  public override method ToString(): string "
        "    return Extra.Render().ToString(); end ToString; "
        "  public constructor(w: Widget4014) "
        "    Extra := w; "
        "    var s := this.ToString(); "
        "  end constructor; "
        "end RecordObsOverrideOk4014; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget4014(42); "
        "    var c := new RecordObsOverrideOk4014(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;"),
        42);
}

// A `this(...)`-chaining record constructor enters its own body with every
// §7.9-strict field of the chained-to constructor already assigned
// (`runRecordConstructor` seeds `assignedFields_` from `fieldsNeedingInit_`
// on a `this(...)` chain, mirroring `runConstructor`) — an instance call in
// the chaining constructor's OWN body must be ACCEPTED.
//
// fix #4095 (§9.7 / §22.7 / §22.8, v0.295): the pre-#4095 shape — a chain
// argument that is a fresh producer (`this(new Widget4014(42))`) — is now
// REJECTED at sema time. The rule inverts: a record constructor's leading
// `this(...)` chain argument MUST NOT be a fresh owned producer flowing
// into a non-`owned` chain-target formal — §9.7's no-temporary-source rule
// applies one level deeper, to the chain's argument list. The observation
// gate's original purpose (instance call AFTER chain-allocated fields are
// seeded) is moot here, because the chain itself never compiles. This row
// now pins the §9.7 REJECT instead of the chain-side observation gate —
// the canonical regression test for fix #4095's record-ctor half. (The
// dedicated §9.7 test in `record_chain_fresh_producer_reject_4095_test.cpp`
// pins the same rule across every other documented arm; this row pins the
// SPECIFIC interaction with the §22.7 observation gate's pre-existing
// ACCEPT path.)
BEZEL_TEST(sema_record_ctor_observation_gate_4014,
           this_chaining_ctor_body_call_accepted_spec_22_7) {
    auto r = runAot(std::string(kWidget4014) +
        "public record RecordObsChain4014 "
        "  public readonly property Extra: Widget4014; "
        "  public constructor(w: Widget4014) Extra := w; end constructor; "
        "  public constructor() "
        "    this(new Widget4014(42)); "
        "    var x := this.Peek(); "
        "  end constructor; "
        "  public method Peek(): int return Extra.Render(); end Peek; "
        "end RecordObsChain4014; "
        "class C "
        "  public static method Main(): int "
        "    var c := new RecordObsChain4014(); "
        "    return c.Peek(); "
        "  end Main; "
        "end C;");
    // fix #4095 (§9.7 / §22.7 / §22.8, v0.295): the chain arg
    // `new Widget4014(42)` is rejected before the observation gate would
    // have a chance to run. The §9.7 diagnostic cites the no-temporary-
    // source rule by name.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "fresh producer"));
    BEZEL_EXPECT_TRUE(diagContains(r, "this(...)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "9.7"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Class control (unaffected by this fix — pins that the class path's
// pre-existing rejection is unchanged): the identical shape on a CLASS was
// already rejected before #4014 and must remain rejected identically.
BEZEL_TEST(sema_record_ctor_observation_gate_4014,
           class_control_instance_method_call_before_field_assigned_rejected) {
    auto r = runAot(std::string(kWidget4014) +
        "public class ClassObsCtor4014 "
        "  public Extra: Widget4014; "
        "  public constructor(w: Widget4014) "
        "    var x := this.Peek(); "
        "    Extra := w; "
        "  end constructor; "
        "  public method Peek(): int return Extra.Render(); end Peek; "
        "end ClassObsCtor4014; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget4014(42); "
        "    var c := new ClassObsCtor4014(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot observe `this`"));
}
