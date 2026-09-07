// Spec-derived tests for fix #4011 — §7.9 (v0.294) no-explicit-constructor
// screen for a NESTED class or record.
//
// §7.9, verbatim (Bezel_Language_Spec.md line ~1117, v0.294):
//   "Every non-nullable field must be either:
//    - assigned a value in every constructor before the constructor returns, or
//    - given a field initializer at its declaration."
//
// Two "no explicit constructor at all" screens exist: Ownership.cpp's
// checkOwnedFieldDA for classes, and DefiniteAssignment.cpp's
// checkRecordNoCtorFieldCompletion (fix #4001) for records. Both were invoked
// ONLY from their pass's checkTopDecl, which walks SourceFile::topDecls
// directly. Neither pass's checkMember ever called its own screen from the
// NestedTypeMember arm — that arm recursed into a nested type's OWN members
// (running constructor bodies, etc.) but never re-ran the type-LEVEL no-ctor
// screen for the nested type itself. The result: a nested class or record
// with a non-nullable field/auto-property and no explicit constructor
// compiled clean and SIGSEGVed on first read, even though the IDENTICAL
// declaration at the TOP level was already correctly rejected. This is
// pre-existing and affects both classes and records identically, since both
// no-ctor screens shared the same checkTopDecl-only invocation shape.
//
// The fix factors each pass's dispatch ("is this a Class (Ownership.cpp) /
// Record (DefiniteAssignment.cpp) and does it need the no-ctor screen?") into
// a small helper, checkNoCtorScreenForType, that BOTH checkTopDecl and the
// NestedTypeMember arm of checkMember call — so a nested type gets the same
// obligation as a top-level one, in each pass.
//
// Because the pre-fix symptom is a SIGSEGV rather than a clean diagnostic-
// free compile, the reject-arm tests use failedAtCompileTimeWithRealLocation
// (aot_result_asserts.hpp) rather than a bare `!r.ran`.

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

const char* kWidget4011 =
    "module M; "
    "public class Widget4011 "
    "  public Count: int; "
    "  public constructor(c: int) Count := c; end constructor; "
    "  public method Render(): int return Count; end Render; "
    "end Widget4011; ";

}  // namespace

// =====================================================================
// REJECT arm — issue #4011's measured repros
// =====================================================================

// A nested CLASS with a non-nullable field and no constructor at all —
// checkOwnedFieldDA's screen must fire for the nested type, not just at the
// top level.
BEZEL_TEST(sema_nested_type_no_ctor_4011,
           nested_class_field_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4011) +
        "public class Outer4011A "
        "  public class Inner4011A "
        "    public Extra: Widget4011; "
        "  end Inner4011A; "
        "end Outer4011A; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Outer4011A.Inner4011A(); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// A nested class with a non-nullable AUTO-PROPERTY and no constructor —
// composes fix #4012 (auto-property coverage) with fix #4011 (nested
// dispatch): both gaps had to close for this shape to be caught.
BEZEL_TEST(sema_nested_type_no_ctor_4011,
           nested_class_autoprop_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4011) +
        "public class Outer4011B "
        "  public class Inner4011B "
        "    public property Extra: Widget4011; "
        "  end Inner4011B; "
        "end Outer4011B; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Outer4011B.Inner4011B(); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// A nested RECORD (combined form, body storage) with no constructor at all —
// checkRecordNoCtorFieldCompletion's screen must fire for the nested type.
BEZEL_TEST(sema_nested_type_no_ctor_4011,
           nested_record_field_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4011) +
        "public class Outer4011C "
        "  public record Inner4011C(X: int) "
        "    public Extra: Widget4011; "
        "  end Inner4011C; "
        "end Outer4011C; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Outer4011C.Inner4011C(5); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// A nested class one level DEEPER (three levels total) — the dispatch must
// recurse, not just fire once at the first nesting level.
BEZEL_TEST(sema_nested_type_no_ctor_4011,
           doubly_nested_class_field_no_ctor_rejected_spec_7_9) {
    auto r = runAot(std::string(kWidget4011) +
        "public class Outer4011D "
        "  public class Middle4011D "
        "    public class Inner4011D "
        "      public Extra: Widget4011; "
        "    end Inner4011D; "
        "  end Middle4011D; "
        "end Outer4011D; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Outer4011D.Middle4011D.Inner4011D(); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// The asymmetry that led to this issue: the identical TOP-LEVEL shape was
// already correctly rejected before this fix — only NESTING bypassed the
// screen. Pinned as a control so a future change cannot silently reopen
// this asymmetry unnoticed.
// =====================================================================

BEZEL_TEST(sema_nested_type_no_ctor_4011,
           top_level_class_field_no_ctor_already_rejected_control_spec_7_9) {
    auto r = runAot(std::string(kWidget4011) +
        "public class TopLevel4011 "
        "  public Extra: Widget4011; "
        "end TopLevel4011; "
        "class C "
        "  public static method Main(): int "
        "    var c := new TopLevel4011(); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `Extra`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§7.9"));
}

// =====================================================================
// ACCEPT arms (over-rejection guards) — a nested class/record WITH a
// constructor that assigns the field is unaffected: each reads back a
// runtime VALUE, not merely a clean compile.
// =====================================================================

BEZEL_TEST(sema_nested_type_no_ctor_4011,
           nested_class_ctor_assigns_field_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4011) +
        "public class Outer4011OK "
        "  public class Inner4011OK "
        "    public Extra: Widget4011; "
        "    public constructor(w: Widget4011) Extra := w; end constructor; "
        "  end Inner4011OK; "
        "end Outer4011OK; "
        "class C "
        "  public static method Main(): int "
        "    using var w := new Widget4011(33); "
        "    var c := new Outer4011OK.Inner4011OK(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;"),
        33);
}

BEZEL_TEST(sema_nested_type_no_ctor_4011,
           nested_record_ctor_assigns_field_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4011) +
        "public class Outer4011ROK "
        "  public record Inner4011ROK "
        "    public readonly property Extra: Widget4011; "
        "    public constructor(w: Widget4011) Extra := w; end constructor; "
        "  end Inner4011ROK; "
        "end Outer4011ROK; "
        "class C "
        "  public static method Main(): int "
        "    using var w := new Widget4011(64); "
        "    var c := new Outer4011ROK.Inner4011ROK(w); "
        "    return c.Extra.Render(); "
        "  end Main; "
        "end C;"),
        64);
}

// A nested class with a NULLABLE field is exempt whether or not a
// constructor exists — nesting must not turn on a rejection an equivalent
// top-level nullable field would not get either.
BEZEL_TEST(sema_nested_type_no_ctor_4011,
           nested_class_nullable_field_no_ctor_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kWidget4011) +
        "public class Outer4011N "
        "  public class Inner4011N "
        "    public Extra: Widget4011?; "
        "  end Inner4011N; "
        "end Outer4011N; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Outer4011N.Inner4011N(); "
        "    return (if c.Extra = null then 1 else 0); "
        "  end Main; "
        "end C;"),
        1);
}
