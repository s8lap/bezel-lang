// Spec-derived tests for fix #4012 — §7.9 (v0.294) definite assignment for a
// CLASS's non-nullable AUTO-PROPERTY with no explicit constructor.
//
// §7.9, verbatim (Bezel_Language_Spec.md line ~1117, v0.294):
//   "Every non-nullable field must be either:
//    - assigned a value in every constructor before the constructor returns, or
//    - given a field initializer at its declaration."
//
// Ownership.cpp's checkOwnedFieldDA is the "no explicit constructor at all"
// §7.9 screen for classes (the per-constructor fall-off completeness check
// lives separately, in DefiniteAssignment.cpp's checkCtorFieldCompletion).
// Before this fix, checkOwnedFieldDA's member loop matched only
// `ast::FieldDecl` — never `ast::PropertyDecl` — so a class's non-nullable
// auto-property storage (`public property Extra: Widget;`) was invisible to
// it. When such a class ALSO declared no explicit constructor, nothing else
// caught the gap either (DefiniteAssignment.cpp's checkMember only walks
// DECLARED constructors, and there is none): `new C()` compiled clean and
// SIGSEGVed on first read of the unset, zero-filled auto-property backing
// storage. The identical shape with a plain FIELD instead of an
// auto-property was ALREADY correctly rejected — confirming the gap was
// specifically the field/auto-property asymmetry in checkOwnedFieldDA's
// member loop, not the underlying §7.9 classification (isSevenNineStrictAuto
// Prop in DefiniteAssignment.cpp already had the right predicate; it was
// simply never consulted from checkOwnedFieldDA's no-ctor path).
//
// The fix adds isNonNullableReferenceAutoProp (Ownership.cpp), a byte-for-
// byte mirror of the existing isNonNullableReferenceField onto
// ast::PropertyDecl — the same duplication-by-design pattern already
// documented for isSevenNineStrictField/isSevenNineStrictAutoProp in
// DefiniteAssignment.cpp — and feeds it into checkOwnedFieldDA's member loop
// alongside the existing FieldDecl arm.
//
// Because the pre-fix symptom is a SIGSEGV rather than a clean diagnostic-
// free compile, the reject-arm tests use failedAtCompileTimeWithRealLocation
// (aot_result_asserts.hpp) rather than a bare `!r.ran`, which a crash/signal
// would satisfy too (a bare BEZEL_EXPECT_FALSE(r.ran) passes WITHOUT the fix
// here, since a SIGSEGV also makes r.ran false).

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

// The class every repro below funnels a non-nullable reference auto-property
// through, shared so every test case reads declaratively.
const char* kWidget4012 =
    "module M; "
    "public class Widget4012 "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget4012; ";

}  // namespace

// =====================================================================
// REJECT arm — issue #4012's measured repro
// =====================================================================

// The issue's own repro: a class whose non-nullable storage is an
// auto-property, with NO explicit constructor at all.
BEZEL_TEST(sema_class_autoprop_da_4012,
           class_autoprop_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4012) +
        "public class Holder4012A "
        "  public property Extra: Widget4012; "
        "end Holder4012A; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Holder4012A(); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// The `readonly` spelling of the same auto-property shape — readonly-ness
// does not change the backing-storage classification.
BEZEL_TEST(sema_class_autoprop_da_4012,
           class_readonly_autoprop_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4012) +
        "public class Holder4012B "
        "  public readonly property Extra: Widget4012; "
        "end Holder4012B; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Holder4012B(); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// The asymmetry that led to this issue: a plain FIELD was ALREADY correctly
// rejected before this fix — only the auto-property form had the gap.
// Pinned here as a control so a future change to either check cannot
// silently reopen this asymmetry unnoticed.
// =====================================================================

BEZEL_TEST(sema_class_autoprop_da_4012,
           class_field_no_ctor_already_rejected_control_spec_7_9) {
    auto r = runAot(std::string(kWidget4012) +
        "public class Holder4012Field "
        "  public Extra: Widget4012; "
        "end Holder4012Field; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Holder4012Field(); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// A class WITH a constructor that omits assigning the auto-property is a
// DIFFERENT, pre-existing screen (DefiniteAssignment.cpp's
// checkCtorFieldCompletion via runConstructor's own isSevenNineStrictAutoProp
// use, unaffected by this fix) — pinned as a control so the two screens'
// division of labor stays visible.
BEZEL_TEST(sema_class_autoprop_da_4012,
           class_autoprop_ctor_omits_assignment_rejected_control_spec_7_9) {
    auto r = runAot(std::string(kWidget4012) +
        "public class Holder4012Omit "
        "  public property Extra: Widget4012; "
        "  public constructor() end constructor; "
        "end Holder4012Omit; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Holder4012Omit(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "constructor does not initialize non-nullable field "
                        "`Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// ACCEPT arms (over-rejection guards) — each reads back a runtime VALUE, not
// merely a clean compile, so a false reject shows as a value/-999 mismatch.
// =====================================================================

// An explicit constructor that DOES assign the auto-property on its
// normal-exit path discharges the obligation and the value round-trips.
BEZEL_TEST(sema_class_autoprop_da_4012,
           class_autoprop_ctor_assigns_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4012) +
        "public class Holder4012OK "
        "  public property Extra: Widget4012; "
        "  public constructor(w: Widget4012) Extra := w; end constructor; "
        "end Holder4012OK; "
        "class C "
        "  public static method Main(): int "
        "    using var w := new Widget4012(42); "
        "    var c := new Holder4012OK(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;"),
        42);
}

// A NULLABLE auto-property has a well-defined default (`null`, §7.9's
// "Default initialization" subsection) and is exempt whether or not a
// constructor exists — must stay accepted, no-ctor included.
BEZEL_TEST(sema_class_autoprop_da_4012,
           nullable_autoprop_no_ctor_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4012) +
        "public class Holder4012Nullable "
        "  public property Extra: Widget4012?; "
        "end Holder4012Nullable; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Holder4012Nullable(); "
        "    return (if c.Extra = null then 1 else 0); "
        "  end Main; "
        "end C;"),
        1);
}

// A VALUE-typed (int) auto-property has a well-defined zero default (§7.9's
// "Default initialization" subsection admits every value type the property
// grants) and is exempt whether or not a constructor exists.
BEZEL_TEST(sema_class_autoprop_da_4012,
           value_typed_autoprop_no_ctor_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Holder4012Value "
        "  public property Count: int; "
        "end Holder4012Value; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Holder4012Value(); "
        "    return c.Count; "
        "  end Main; "
        "end C;"),
        0);
}

// An auto-property with an INITIALIZER discharges the §7.9 obligation
// exactly as a field initializer does — assigned at construction, no
// constructor needed.
BEZEL_TEST(sema_class_autoprop_da_4012,
           autoprop_initializer_discharges_obligation_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Holder4012Init "
        "  public property Extra: string := \"hello\"; "
        "end Holder4012Init; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Holder4012Init(); "
        "    return c.Extra.Length; "
        "  end Main; "
        "end C;"),
        5);
}

// A GENERIC class whose auto-property's declared type is a bare, unresolved
// type parameter `T` is conservatively NOT flagged — matching the existing,
// pre-fix (and unchanged by this fix) treatment of a bare-`T` FIELD, per
// isNonNullableReferenceField's own documented posture ("Generic-parameter
// and unresolved types are conservatively treated as non-reference"). Pinned
// as a control so this fix does not silently narrow that existing, symmetric
// exemption to fields only.
BEZEL_TEST(sema_class_autoprop_da_4012,
           generic_bare_t_autoprop_no_ctor_accepted_control_spec_7_10) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Box4012<T> where T: class "
        "  public property Item: T; "
        "end Box4012; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box4012<string>(); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}
