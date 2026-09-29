// Spec-derived tests for fix #4001 — §7.9 (v0.294) definite assignment for a
// record's BODY field / auto-property.
//
// §7.9, verbatim (Bezel_Language_Spec.md line ~1117, v0.294):
//   "Every non-nullable field must be either:
//    - assigned a value in every constructor before the constructor returns, or
//    - given a field initializer at its declaration."
// Nothing in that text scopes the rule to classes. §9.4 (line ~1452) makes the
// record reading explicit: "a record constructor initializes the value under
// construction — `Width := width;` is §9.2's own canonical form, governed by
// §22.8's once-only and §7.9's definite-assignment machinery" — so a record
// constructor's field stores are governed by the SAME §7.9 machinery a class
// constructor's are.
//
// Before this fix, `DefiniteAssignment.cpp`'s `checkMember` recorded the
// OPPOSITE premise in its record-constructor branch: "§7.9 applies to class
// instances, not record values — records are value types with no per-instance
// non-nullable-field set to check." That premise does not match the spec text
// above, and its practical consequence was a compiler-verified crash: a body
// field or auto-property of non-nullable, non-defaultable type
// (§7.9's "Default initialization" subsection, §17.11) that no constructor
// assigns is left at its zero-filled slot on `new R(...)`, and reading it back
// SIGSEGVs on first dereference — exactly the same "unset non-nullable field"
// state §7.9 forbids for a class, reached through the far more common ordinary
// `new R(...)` construction path rather than `default(T)`.
//
// This file pins THREE independently-measured pre-fix crash shapes (issue
// #4001's own repros) as compile-time rejections, plus the positional-field
// asymmetry that was the issue's own diagnostic lead (a positional field is
// ALREADY correctly enforced via the required-parameter check, §22.5 — the gap
// was body storage only), plus over-rejection guards (nullable field, value-
// typed field, field initializer, explicit assignment, `this(...)` chaining,
// multiple constructors) so a false reject would show here as a value/compile
// mismatch rather than passing silently. Because the pre-fix symptom is a
// SIGSEGV rather than a clean diagnostic-free compile, the reject-arm tests use
// `failedAtCompileTimeWithRealLocation` (aot_result_asserts.hpp) rather than a
// bare `!r.ran`, which a crash/signal/timeout would satisfy too.

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

// The class every repro below funnels a non-nullable reference field through,
// shared so every test case reads declaratively.
const char* kWidget =
    "module M; "
    "public class Widget4001 "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget4001; ";

}  // namespace

// =====================================================================
// REJECT arm — issue #4001's three measured repros
// =====================================================================

// Repro 1: a combined-form record's EXPLICIT (non-property) body field, with
// NO constructor declared at all. The auto-generated positional constructor
// binds only the positional field (§9.1) and never touches `Extra` — the
// no-explicit-constructor screen (checkRecordNoCtorFieldCompletion) must fire.
BEZEL_TEST(sema_record_field_da_4001,
           combined_form_explicit_body_field_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget) +
        "public record Combo4001A(X: int) "
        "  public Extra: Widget4001; "
        "end Combo4001A; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Combo4001A(5); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// Repro 2: the same shape with a proper `readonly property` auto-property in
// place of the explicit field — the auto-property's backing storage carries
// the identical §7.9 obligation (§22.7: "auto-property assignments and reads
// … each [are] the backing field's assignment or read").
BEZEL_TEST(sema_record_field_da_4001,
           combined_form_autoprop_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget) +
        "public record Combo4001B(X: int) "
        "  public readonly property Extra: Widget4001; "
        "end Combo4001B; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Combo4001B(5); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// Repro 3: a body-only record WITH an explicit user constructor that simply
// never assigns the field — rules out "records only ever get an auto-
// generated ctor" as the trigger. This is `runRecordConstructor`'s own
// fall-off completeness check (`recordCtorMode_`/`checkCtorFieldCompletion`),
// not the no-ctor screen (a constructor IS present).
BEZEL_TEST(sema_record_field_da_4001,
           body_only_explicit_ctor_leaves_field_unassigned_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget) +
        "public record BodyOnly4001C "
        "  public readonly property Extra: Widget4001; "
        "  public constructor(unrelated: int) "
        "  end constructor; "
        "end BodyOnly4001C; "
        "class C "
        "  public static method Main(): int "
        "    var c := new BodyOnly4001C(5); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "constructor does not initialize non-nullable field "
                        "`Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// A record with SEVERAL constructors: one assigns the field, one does not.
// Each declared constructor carries its OWN §7.9 obligation (mirroring the
// class rule) — the well-formed one must not mask the other.
BEZEL_TEST(sema_record_field_da_4001,
           one_of_several_ctors_leaves_field_unassigned_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget) +
        "public record MultiCtor4001 "
        "  public readonly property Extra: Widget4001; "
        "  public constructor(w: Widget4001) Extra := w; end constructor; "
        "  public constructor(unused: int) end constructor; "
        "end MultiCtor4001; "
        "class C "
        "  public static method Main(): int "
        "    var c := new MultiCtor4001(3); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "constructor does not initialize non-nullable field "
                        "`Extra`"));
}

// =====================================================================
// The asymmetry that led to this issue: a POSITIONAL field was ALREADY
// correctly enforced (§22.5's required-parameter check) before this fix —
// only body storage had the gap. Pinned here as a control so a future change
// to either check cannot silently reopen this asymmetry unnoticed.
// =====================================================================

BEZEL_TEST(sema_record_field_da_4001,
           positional_field_omission_already_rejected_spec_22_5) {
    auto r = runAot(std::string(kWidget) +
        "public record Positional4001(Extra: Widget4001) "
        "end Positional4001; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget4001(5); "
        "    var c := new Positional4001(); "  // omits the required arg
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.5"));
}

// =====================================================================
// ACCEPT arms (over-rejection guards) — each reads back a runtime VALUE, not
// merely a clean compile, so a false reject shows as a value/-999 mismatch.
// =====================================================================

// A field initializer discharges the §7.9 obligation for a record exactly as
// it does for a class (§22.8: "A field declared in a class OR RECORD may
// include an initializer").
BEZEL_TEST(sema_record_field_da_4001,
           field_initializer_discharges_obligation_accepted_spec_22_8) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record WithInit4001 "
        "  public Extra: string := \"hello\"; "
        "end WithInit4001; "
        "class C "
        "  public static method Main(): int "
        "    var c := new WithInit4001(); "
        "    return c.Extra.Length; "
        "  end Main; "
        "end C;"),
        5);
}

// An explicit constructor that DOES assign the auto-property on its
// normal-exit path discharges the obligation and the value round-trips.
BEZEL_TEST(sema_record_field_da_4001,
           explicit_ctor_assigns_field_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget) +
        "public record BodyOnly4001OK "
        "  public readonly property Extra: Widget4001; "
        "  public constructor(w: Widget4001) Extra := w; end constructor; "
        "end BodyOnly4001OK; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget4001(77); "
        "    var c := new BodyOnly4001OK(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;"),
        77);
}

// FLIPPED for fix #5132 (§9.3 / §11.1 / §22.6, v0.308). The original row
// pinned `record Combo4001OK(X: int)` + `constructor(x: int)` — a body
// ctor whose parameter list differs from the primary only by name — as
// an ACCEPT oracle ("combined form explicitly supplies a body for the
// primary"). Under the v0.307 predicate (equal arity + equal names +
// equal types + equal default presence + equal §22.6 modifier) the name
// mismatch made it not-an-exact-duplicate, so it compiled; under v0.308
// (overload identity — names do not distinguish) the body ctor and the
// primary are indistinguishable and §9.3 rejects the declaration. The
// reject oracle here names the §9.3 redeclares-primary diagnostic; the
// row's RUNTIME check (return 10) is replaced with a compile-time
// check. The companion `field_initializer_discharges_obligation_accepted`
// and the nullable / value-typed body-field rows below still pin the
// §7.9 obligation rule unchanged.
BEZEL_TEST(sema_record_field_da_4001,
           combined_form_primary_ctor_body_clashes_with_primary_rejected_spec_9_3) {
    auto r = runAot(std::string(kWidget) +
        "public record Combo4001OK(X: int) "
        "  public readonly property Extra: Widget4001; "
        "  public constructor(x: int) "
        "    X := x; "
        "    Extra := new Widget4001(x * 2); "
        "  end constructor; "
        "end Combo4001OK; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Combo4001OK(5); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "body constructor redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7""9.3"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A NULLABLE body field has a well-defined default (`null`, §7.9's "Default
// initialization" subsection) and is exempt whether or not a constructor
// exists — must stay accepted.
BEZEL_TEST(sema_record_field_da_4001,
           nullable_body_field_no_ctor_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget) +
        "public record Nullable4001(X: int) "
        "  public Extra: Widget4001?; "
        "end Nullable4001; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Nullable4001(5); "
        "    return (if c.Extra = null then 1 else 0); "
        "  end Main; "
        "end C;"),
        1);
}

// A VALUE-typed (record) body field has a well-defined zero default (§7.9's
// "Default initialization" subsection admits every value type the property
// grants) and is exempt whether or not a constructor exists.
BEZEL_TEST(sema_record_field_da_4001,
           value_typed_body_field_no_ctor_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record Inner4001(A: int, B: int); "
        "public record Outer4001(X: int) "
        "  public Extra: Inner4001; "
        "end Outer4001; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Outer4001(5); "
        "    return c.Extra.A; "
        "  end Main; "
        "end C;"),
        0);
}
