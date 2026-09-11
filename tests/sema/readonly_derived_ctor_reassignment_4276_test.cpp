// Spec-derived tests for fix #4276 — §22.8 "Field initializers" / §23.1
// "Auto-properties" (v0.298), and a REFUTATION of the sibling issue #4275.
//
// Both issues were filed by the lane implementing fix #4253 (codegen: class-
// field record-string-field release), which needed a `sealed` +
// `!hasAnyConstructor` guard specifically because these two shapes are
// currently accepted with no diagnostic. #4275 claimed a readonly PROPERTY
// with a declaration initializer, reassigned again in its OWN class's
// constructor body, "should be rejected per §22.8/§23.1". That claim does
// NOT survive reading §22.8 to its own scope statement, or DECLINED.md:
//
//   "The single-assignment rule (instance and static readonly FIELDS)...
//    Scope of this rule: it governs readonly fields. Whether it also
//    governs a readonly auto-property (§23.1) is not decided here;
//    extending it there is a separate ruling, DECLINED with its reasons
//    recorded (v0.271)." (§22.8, v0.298)
//   "A readonly auto-property may be assigned only in a field initializer
//    or in a constructor... that section's single-assignment rule governs
//    fields and does not extend here (its scope statement)." (§23.1, v0.298)
//   DECLINED.md: "Extending §22.8's single-assignment rule to readonly
//    auto-properties" | rejected | "...The reference implementation
//    deliberately admits an initializer followed by a constructor-body
//    store on a readonly auto-property, the body store overriding the
//    initializer — established by #3207's fix and asserted twice with
//    runtime values in tests/module/xmod_record_body_ctor_field_store_
//    3207_test.cpp."
//
// So #4275's shape is DELIBERATE, RULED behaviour, not a gap — "fixing" it
// would silently reverse a recorded decline and break the #3207 test's own
// pinned runtime assertion. Row `same_class_ctor_body_reassignment_over_
// initializer_still_accepted_refutes_issue_4275` below pins the ACCEPT side
// so this cannot regress by accident; issue #4275 is closed as refuted (see
// the commit body for the gh issue comment).
//
// #4276 is a DIFFERENT question — not counting HOW MANY times a readonly
// property/field is assigned, but WHETHER a constructor outside the
// DECLARING type may assign it at all. §22.8's own text states the field
// rule's per-instance carve-out as "assignable only through an lvalue chain
// rooted at `this`" with NO additional declaring-type qualifier for the
// INSTANCE case (only the `static readonly` case is explicitly scoped to
// "a constructor body of the declaring type") — but the compiler's OWN
// existing implementation of the BARE-identifier spelling of the same store
// (`P := e`, no `this.`) already requires `ps->parent == currentType_`
// (`ExprTypecheck.cpp: isThisRootedLValueTarget`'s bare-identifier branch).
// The EXPLICIT `this.P := e` spelling omitted that same check — MEASURED:
// the two spellings of the identical store, from the identical derived
// constructor, disagreed (bare: rejected; explicit: accepted). That is an
// internal inconsistency independent of any spec ruling: two spellings of
// one store cannot legitimately differ. The fix makes the explicit spelling
// consult the same fact (`currentType_ == fs->parent` / `ps->parent`) the
// bare spelling already does, at all four call sites that gate an instance
// readonly write (`checkReadonlyAssignment`'s field branch,
// `checkPropertyWriteTarget`'s auto-property branch — shared by `:=` and
// `++`/`--` per fix #2868 — and the `checkRefOutFieldOperand`/
// `checkRefOutPropertyOperand` ref/out twins).

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

}  // namespace

// =====================================================================
// REJECT — issue #4276's own repro and its siblings across the four
// gated call sites (`:=` on a property, `:=` on a field, `ref` on a
// property, `ref` on a field) plus a `++` row proving the shared
// `checkPropertyWriteTarget` path (fix #2868) is covered by the same fix.
// =====================================================================

// The issue's own measured shape: a DERIVED class's constructor reassigns
// a readonly AUTO-PROPERTY it inherits from its base, through the explicit
// `this.P` spelling.
BEZEL_TEST(readonly_derived_ctor_reassignment_4276,
           derived_ctor_this_dot_reassigns_inherited_readonly_property_rejected_spec_23_1) {
    auto r = runAot(
        "module M; record Box(Value: string); "
        "class Base "
        "  public readonly property P: Box := new Box(\"x\"); "
        "end Base; "
        "class Derived: Base "
        "  public constructor() "
        "    this.P := new Box(\"y\"); "
        "  end constructor; "
        "end Derived; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot assign to readonly property `P`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§23.1"));
}

// CONSISTENCY GUARD: the bare-identifier spelling of the IDENTICAL store
// (`P := e`, no `this.`) was ALREADY rejected before this fix — pinning it
// here proves the fix makes the explicit spelling agree with this one,
// not the other way around.
BEZEL_TEST(readonly_derived_ctor_reassignment_4276,
           derived_ctor_bare_reassigns_inherited_readonly_property_rejected_spec_23_1) {
    auto r = runAot(
        "module M; record Box(Value: string); "
        "class Base "
        "  public readonly property P: Box := new Box(\"x\"); "
        "end Base; "
        "class Derived: Base "
        "  public constructor() "
        "    P := new Box(\"y\"); "
        "  end constructor; "
        "end Derived; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot assign to readonly property `P`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§23.1"));
}

// The plain-FIELD twin (not an auto-property): `checkReadonlyAssignment`'s
// field branch is the sibling site this fix also narrows. `_f` must be
// `protected` (or wider) for `Derived` to name it at all.
BEZEL_TEST(readonly_derived_ctor_reassignment_4276,
           derived_ctor_this_dot_reassigns_inherited_readonly_field_rejected_spec_22_8) {
    auto r = runAot(
        "module M; "
        "class Base "
        "  protected readonly _f: int := 1; "
        "end Base; "
        "class Derived: Base "
        "  public constructor() "
        "    this._f := 2; "
        "  end constructor; "
        "end Derived; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot assign to readonly field `_f`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.8"));
}

// The `ref`-argument twin of the property row: §22.3 "inherits this
// context unchanged" per §22.8's own text, so binding an inherited
// readonly property to a `ref` parameter from a derived constructor is the
// same store by other syntax — `checkRefOutPropertyOperand`.
BEZEL_TEST(readonly_derived_ctor_reassignment_4276,
           derived_ctor_ref_arg_inherited_readonly_property_rejected_spec_22_3) {
    auto r = runAot(
        "module M; record Box(Value: string); "
        "class Base "
        "  public readonly property P: Box := new Box(\"x\"); "
        "end Base; "
        "class Derived: Base "
        "  public constructor() "
        "    Sink.Touch(ref this.P); "
        "  end constructor; "
        "end Derived; "
        "class Sink "
        "  public static method Touch(ref b: Box) "
        "  end Touch; "
        "end Sink; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "readonly property's setter"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§23.1"));
}

// The `ref`-argument twin of the field row — `checkRefOutFieldOperand`.
BEZEL_TEST(readonly_derived_ctor_reassignment_4276,
           derived_ctor_ref_arg_inherited_readonly_field_rejected_spec_22_3) {
    auto r = runAot(
        "module M; "
        "class Base "
        "  protected readonly _f: int := 1; "
        "end Base; "
        "class Derived: Base "
        "  public constructor() "
        "    Sink.Touch(ref this._f); "
        "  end constructor; "
        "end Derived; "
        "class Sink "
        "  public static method Touch(ref x: int) "
        "  end Touch; "
        "end Sink; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "may not be readonly field `_f`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.8"));
}

// §17.6 / fix #2868: `++`/`--` shares `checkPropertyWriteTarget` with
// `:=` — this row proves the SAME fix (one predicate, not a second copy)
// covers the increment/decrement spelling of the identical store.
BEZEL_TEST(readonly_derived_ctor_reassignment_4276,
           derived_ctor_increment_inherited_readonly_property_rejected_spec_17_6) {
    auto r = runAot(
        "module M; "
        "class Base "
        "  public readonly property N: int := 1; "
        "end Base; "
        "class Derived: Base "
        "  public constructor() "
        "    this.N++; "
        "  end constructor; "
        "end Derived; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot increment/decrement readonly property `N`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.6"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§23.1"));
}

// =====================================================================
// ACCEPT controls (LANE_RULES §15) — the legal shapes adjacent to what is
// now rejected must keep compiling, AND keep producing the correct
// runtime value (not merely "did not reject").
// =====================================================================

// Initializer-only, no constructor at all.
BEZEL_TEST(readonly_derived_ctor_reassignment_4276,
           same_class_initializer_only_no_constructor_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; record Box(Value: string); "
        "class Holder "
        "  public readonly property P: Box := new Box(\"x\"); "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  return h.P.Value.Length; "
        "end Main; end C;"), 1);
}

// Constructor-only assignment, no declaration initializer.
BEZEL_TEST(readonly_derived_ctor_reassignment_4276,
           same_class_constructor_only_assignment_no_initializer_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; record Box(Value: string); "
        "class Holder "
        "  public readonly property P: Box; "
        "  public constructor() "
        "    this.P := new Box(\"y\"); "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  return h.P.Value.Length; "
        "end Main; end C;"), 1);
}

// A `this(...)` delegation chain where exactly ONE link assigns the
// property (the chaining constructor's own body never touches it).
BEZEL_TEST(readonly_derived_ctor_reassignment_4276,
           same_class_this_chain_exactly_one_link_assigns_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; record Box(Value: string); "
        "class Holder "
        "  public readonly property P: Box; "
        "  public constructor() "
        "    this(new Box(\"chained\")); "
        "  end constructor; "
        "  public constructor(b: Box) "
        "    this.P := b; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  return h.P.Value.Length; "
        "end Main; end C;"), 7);
}

// A derived class WITH its own constructor that never touches the
// inherited readonly property at all — must keep compiling and reading
// the base's own initializer value.
BEZEL_TEST(readonly_derived_ctor_reassignment_4276,
           derived_class_does_not_touch_base_readonly_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; record Box(Value: string); "
        "class Base "
        "  public readonly property P: Box := new Box(\"basev\"); "
        "end Base; "
        "class Derived: Base "
        "  public constructor() "
        "  end constructor; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  return d.P.Value.Length; "
        "end Main; end C;"), 5);
}

// CRITICAL CONTROL, refuting issue #4275: a readonly property WITH a
// declaration initializer, reassigned again in its OWN class's own
// constructor body, must STILL compile and the reassignment must STILL
// take effect at runtime — this is the deliberately-declined shape
// (DECLINED.md, v0.271; see file header). Same class as #4275's own
// repro, `currentType_ == ps->parent` holds, so this fix does not (and
// must not) touch it.
BEZEL_TEST(readonly_derived_ctor_reassignment_4276,
           same_class_ctor_body_reassignment_over_initializer_still_accepted_refutes_issue_4275) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; record Box(Value: string); "
        "class Holder "
        "  public readonly property P: Box := new Box(\"first\"); "
        "  public constructor() "
        "    this.P := new Box(\"second\"); "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  return h.P.Value.Length; "
        "end Main; end C;"), 6);
}

// A derived class that declares its OWN readonly property (distinct from
// the inherited one) and assigns it in its own constructor — proves the
// fix keys on the ASSIGNED member's declaring type, not on "any
// inheritance relationship with the current type", so ordinary same-type
// property assignment inside a subclass is unaffected.
BEZEL_TEST(readonly_derived_ctor_reassignment_4276,
           derived_class_assigns_its_own_readonly_property_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; record Box(Value: string); "
        "class Base "
        "  public readonly property P: Box := new Box(\"basev\"); "
        "end Base; "
        "class Derived: Base "
        "  public readonly property Q: Box; "
        "  public constructor() "
        "    this.Q := new Box(\"qval\"); "
        "  end constructor; "
        "end Derived; "
        "class C public static method Main(): int "
        "  using var d := new Derived(); "
        "  return d.P.Value.Length + d.Q.Value.Length; "
        "end Main; end C;"), 9);
}
