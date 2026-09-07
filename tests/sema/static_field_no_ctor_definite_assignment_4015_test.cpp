// Spec-derived tests for fix #4015 — §7.9 (v0.294) definite assignment for a
// STATIC non-nullable field/auto-property, on both classes and records.
//
// §7.9's "Default initialization" subsection, verbatim (Bezel_Language_Spec.md
// line ~1128, v0.294):
//   "A field whose type is not defaultable ... requires an explicit
//    initializer: a field initializer, or assignment in every constructor
//    (for instance fields) or in the static constructor (for static fields).
//    The compiler emits a definite-assignment error if neither path supplies
//    a value."
//
// Before this fix, NOTHING screened the static half of that sentence, for
// either type kind:
//   - `isSevenNineStrictField`/`isSevenNineStrictAutoProp`
//     (DefiniteAssignment.cpp) and `isNonNullableReferenceField`/
//     `isNonNullableReferenceAutoProp` (Ownership.cpp) all explicitly EXCLUDE
//     `static` members from their classification.
//   - `runStaticConstructor` (DefiniteAssignment.cpp) walked a declared
//     static constructor's body for `readonly` single-assignment tracking
//     ONLY (fix #2187) — it never populated `fieldsNeedingInit_`/
//     `assignedFields_` or called `checkCtorFieldCompletion`, so even a
//     DECLARED static constructor that never got around to assigning a
//     `static readonly` field was never caught.
//   - `checkMember`'s ConstructorDecl dispatch had no branch at all for
//     `cd->isStatic && inRecord` — a record's static constructor fell to the
//     untyped `da.run` path, which gave it NEITHER the §7.9 completion check
//     NOR the pre-existing §22.8 readonly single-assignment check a class's
//     static constructor already had.
//
// The practical consequence, MEASURED on both type kinds identically: a
// `static readonly` field of non-defaultable type with no static constructor
// and no initializer compiled with no §7.9 diagnostic and SIGSEGVed on first
// read (issue #4015's own repro), and a record's static constructor could
// double-assign a `static readonly` field with no §22.8 diagnostic (a
// separate, sibling gap this fix closes via the same missing-dispatch-branch
// root cause rather than as a distinct issue).
//
// The fix adds two new predicates (isSevenNineStrictStaticField/
// isSevenNineStrictStaticAutoProp, DefiniteAssignment.cpp) — deliberately
// SEPARATE functions rather than parameters threaded onto the existing
// instance-side ones, per this run's own note that a static auto-property
// predicate must independently exclude the `abstract` shorthand (§12.5)
// landmine tracked by issues #4018/#4020 — a new no-ctor screen
// (checkStaticNoCtorFieldCompletion) run for both classes and records from
// checkNoCtorScreenForType (and therefore, for free, from its existing
// nested-type dispatch, fix #4011), and extends runStaticConstructor with the
// same fieldsNeedingInit_/assignedFields_/checkCtorFieldCompletion tracking
// runConstructor/runRecordConstructor already have, now also reached by a
// record's static constructor via a new checkMember branch.
//
// Because the pre-fix symptom is a SIGSEGV rather than a clean diagnostic-
// free compile, every reject-arm test below uses
// `failedAtCompileTimeWithRealLocation` (aot_result_asserts.hpp) rather than a
// bare `!r.ran`, which a crash/signal would satisfy too.

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

// The class every repro below funnels a non-nullable static field through,
// shared so every test case reads declaratively. Non-`static` on purpose —
// this is the type the STATIC storage under test points AT, not the type
// declaring the static storage.
const char* kWidget4015 =
    "module M; "
    "public class Widget4015 "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget4015; ";

}  // namespace

// =====================================================================
// REJECT arm 1 — issue #4015's own measured repros: `static readonly` field,
// no static constructor, no initializer. Class and record.
// =====================================================================

BEZEL_TEST(sema_static_field_da_4015,
           class_static_readonly_field_no_static_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4015) +
        "public class StaticClass4015A "
        "  public static readonly Shared: Widget4015; "
        "end StaticClass4015A; "
        "class C "
        "  public static method Main(): int "
        "    return StaticClass4015A.Shared.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable static field `Shared`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

BEZEL_TEST(sema_static_field_da_4015,
           record_static_readonly_field_no_static_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4015) +
        "public record StaticRec4015A(X: int) "
        "  public static readonly Shared: Widget4015; "
        "end StaticRec4015A; "
        "class C "
        "  public static method Main(): int "
        "    var r := new StaticRec4015A(5); "
        "    return StaticRec4015A.Shared.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable static field `Shared`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// REJECT arm 2 — the same no-ctor gap, on a static AUTO-PROPERTY instead of a
// field. Class and record.
// =====================================================================

BEZEL_TEST(sema_static_field_da_4015,
           class_static_readonly_autoprop_no_static_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4015) +
        "public class StaticClass4015B "
        "  public static readonly property Shared: Widget4015; "
        "end StaticClass4015B; "
        "class C "
        "  public static method Main(): int "
        "    return StaticClass4015B.Shared.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable static field `Shared`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

BEZEL_TEST(sema_static_field_da_4015,
           record_static_readonly_autoprop_no_static_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4015) +
        "public record StaticRec4015B(X: int) "
        "  public static readonly property Shared: Widget4015; "
        "end StaticRec4015B; "
        "class C "
        "  public static method Main(): int "
        "    var r := new StaticRec4015B(5); "
        "    return StaticRec4015B.Shared.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable static field `Shared`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// REJECT arm 3 — a static constructor IS declared but its normal-exit path
// never assigns the field (the fall-off completeness gap in
// runStaticConstructor, distinct from the no-ctor-at-all screen above).
// Class and record.
// =====================================================================

BEZEL_TEST(sema_static_field_da_4015,
           class_static_ctor_omits_assignment_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4015) +
        "public class StaticClass4015C "
        "  public static readonly Shared: Widget4015; "
        "  public static Other: int; "
        "  static constructor() "
        "    Other := 5; "
        "  end constructor; "
        "end StaticClass4015C; "
        "class C "
        "  public static method Main(): int "
        "    return StaticClass4015C.Shared.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "constructor does not initialize non-nullable "
                        "field `Shared`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

BEZEL_TEST(sema_static_field_da_4015,
           record_static_ctor_omits_assignment_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4015) +
        "public record StaticRec4015C(X: int) "
        "  public static readonly Shared: Widget4015; "
        "  public static Other: int; "
        "  static constructor() "
        "    Other := 5; "
        "  end constructor; "
        "end StaticRec4015C; "
        "class C "
        "  public static method Main(): int "
        "    var r := new StaticRec4015C(5); "
        "    return StaticRec4015C.Shared.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "constructor does not initialize non-nullable "
                        "field `Shared`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// REJECT arm 4 — sibling gap closed by the same fix: a RECORD's static
// constructor previously matched no `checkMember` branch at all and fell to
// the untyped local-DA walk, so it got no §22.8 readonly single-assignment
// tracking either. Pinned here (not filed separately) since it is the
// identical missing-dispatch-branch root cause this fix's checkMember change
// closes. The class analogue (`class_static_ctor_double_assign_...`) is
// already covered by pre-existing fix #2187 tests; included here too as a
// same-file control so the record/class parity is visible in one place.
// =====================================================================

BEZEL_TEST(sema_static_field_da_4015,
           record_static_ctor_double_assign_rejected_spec_22_8) {
    auto r = runAot(std::string(kWidget4015) +
        "public record StaticRec4015D(X: int) "
        "  public static readonly Shared: Widget4015; "
        "  static constructor() "
        "    Shared := new Widget4015(1); "
        "    Shared := new Widget4015(2); "
        "  end constructor; "
        "end StaticRec4015D; "
        "class C "
        "  public static method Main(): int "
        "    var r := new StaticRec4015D(5); "
        "    return StaticRec4015D.Shared.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "readonly field `Shared`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.8"));
}

BEZEL_TEST(sema_static_field_da_4015,
           class_static_ctor_double_assign_rejected_control_spec_22_8) {
    auto r = runAot(std::string(kWidget4015) +
        "public class StaticClass4015D "
        "  public static readonly Shared: Widget4015; "
        "  static constructor() "
        "    Shared := new Widget4015(1); "
        "    Shared := new Widget4015(2); "
        "  end constructor; "
        "end StaticClass4015D; "
        "class C "
        "  public static method Main(): int "
        "    return StaticClass4015D.Shared.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "readonly field `Shared`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.8"));
}

// =====================================================================
// ACCEPT arms (over-rejection guards) — each reads back a runtime VALUE, not
// merely a clean compile, so a false reject shows as a value/-999 mismatch.
// =====================================================================

// A static field initializer discharges the obligation with no static
// constructor at all. Class and record.
BEZEL_TEST(sema_static_field_da_4015,
           class_static_readonly_field_initializer_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4015) +
        "public class StaticClass4015E "
        "  public static readonly Shared: Widget4015 := new Widget4015(7); "
        "end StaticClass4015E; "
        "class C "
        "  public static method Main(): int "
        "    return StaticClass4015E.Shared.Render(); "
        "  end Main; "
        "end C;"),
        7);
}

BEZEL_TEST(sema_static_field_da_4015,
           record_static_readonly_field_initializer_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4015) +
        "public record StaticRec4015E(X: int) "
        "  public static readonly Shared: Widget4015 := new Widget4015(7); "
        "end StaticRec4015E; "
        "class C "
        "  public static method Main(): int "
        "    var r := new StaticRec4015E(5); "
        "    return StaticRec4015E.Shared.Render(); "
        "  end Main; "
        "end C;"),
        7);
}

// A declared static constructor that DOES assign the field on every
// normal-exit path discharges the obligation. Class and record — the record
// case also proves runStaticConstructor's new record dispatch (checkMember)
// runs the walk at all rather than silently no-oping.
BEZEL_TEST(sema_static_field_da_4015,
           class_static_ctor_assigns_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4015) +
        "public class StaticClass4015F "
        "  public static readonly Shared: Widget4015; "
        "  static constructor() "
        "    Shared := new Widget4015(8); "
        "  end constructor; "
        "end StaticClass4015F; "
        "class C "
        "  public static method Main(): int "
        "    return StaticClass4015F.Shared.Render(); "
        "  end Main; "
        "end C;"),
        8);
}

BEZEL_TEST(sema_static_field_da_4015,
           record_static_ctor_assigns_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4015) +
        "public record StaticRec4015F(X: int) "
        "  public static readonly Shared: Widget4015; "
        "  static constructor() "
        "    Shared := new Widget4015(9); "
        "  end constructor; "
        "end StaticRec4015F; "
        "class C "
        "  public static method Main(): int "
        "    var r := new StaticRec4015F(5); "
        "    return StaticRec4015F.Shared.Render(); "
        "  end Main; "
        "end C;"),
        9);
}

// A NULLABLE static field has a well-defined default (`null`) and is exempt
// whether or not a static constructor exists.
BEZEL_TEST(sema_static_field_da_4015,
           nullable_static_field_no_static_ctor_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4015) +
        "public class StaticClass4015G "
        "  public static readonly Shared: Widget4015?; "
        "end StaticClass4015G; "
        "class C "
        "  public static method Main(): int "
        "    return (if StaticClass4015G.Shared = null then 1 else 0); "
        "  end Main; "
        "end C;"),
        1);
}

// A VALUE-typed (int) static field has a well-defined zero default and is
// exempt whether or not a static constructor exists. `static readonly` (not
// plain `static`) so no unrelated §22.12 warning noise.
BEZEL_TEST(sema_static_field_da_4015,
           value_typed_static_field_no_static_ctor_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class StaticClass4015H "
        "  public static readonly Count: int; "
        "end StaticClass4015H; "
        "class C "
        "  public static method Main(): int "
        "    return StaticClass4015H.Count; "
        "  end Main; "
        "end C;"),
        0);
}

// =====================================================================
// Nested types — the no-ctor screen (checkStaticNoCtorFieldCompletion) is
// reached via checkNoCtorScreenForType, which fix #4011 already dispatches
// from checkMember's NestedTypeMember arm; this pins that the STATIC screen
// rides that SAME nested dispatch rather than only ever firing at the top
// level.
// =====================================================================

BEZEL_TEST(sema_static_field_da_4015,
           nested_class_static_readonly_field_no_static_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4015) +
        "public class Outer4015 "
        "  public class Inner4015A "
        "    public static readonly Shared: Widget4015; "
        "  end Inner4015A; "
        "end Outer4015; "
        "class C "
        "  public static method Main(): int "
        "    return Outer4015.Inner4015A.Shared.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable static field `Shared`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

BEZEL_TEST(
    sema_static_field_da_4015,
    nested_record_static_readonly_field_no_static_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4015) +
        "public class Outer4015B "
        "  public record Inner4015B(X: int) "
        "    public static readonly Shared: Widget4015; "
        "  end Inner4015B; "
        "end Outer4015B; "
        "class C "
        "  public static method Main(): int "
        "    var r := new Outer4015B.Inner4015B(5); "
        "    return Outer4015B.Inner4015B.Shared.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable static field `Shared`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}
