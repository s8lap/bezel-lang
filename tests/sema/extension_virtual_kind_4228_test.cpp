// Spec-derived tests for fix #4228 — §26.3 "Restrictions" states plainly:
//   "No `virtual`, `override`, `abstract`, or `sealed` — extensions are
//   statically dispatched."
// Appendix A.4 states the grammar this bullet is rejecting semantically:
//   VirtualKind ::= "abstract" | "virtual" | "sealed"? "override"
// `MethodDecl`/`PropertyDecl` admit `VirtualKind?` unconditionally in the
// shared grammar production — an extension member is one of the two
// `ExtensionMember` alternatives (`MethodDecl | ExpressionPropertyDecl`), so
// the grammar over-admits the modifier and this bullet's enforcement has to
// be semantic, the same split §26.3's own "No `static` members" bullet
// already uses (fix #1581/v0.160) and that the `ExtensionMember` A.4 comment
// documents explicitly for `static`.
//
// MEASURED against r18 base `6f46a73` (this run, #4213/#4204 applied,
// pre-#4228): the issue's own repro (a `public virtual method` in an
// extension body) compiled at exit 0 with ZERO diagnostics, linked, and ran.
// This fix's own measurement swept all 4 `VirtualKind` spellings the grammar
// admits (`abstract`, `virtual`, `override`, `sealed override`) against BOTH
// admitted `ExtensionMember` alternatives (method, expression-bodied
// property) — 8 rows total, all 8 confirmed exit 0 / zero diagnostics
// pre-fix (the issue itself measured only 1 of the 8).
//
// THE FIX: `ExprTC::visitTopDecl`'s `ExtensionDecl` arm's per-member
// restriction loop (`src/sema/ExprTypecheck.cpp`, the same loop fix #4213/
// #4204 extended) gets a new modifier check reading the `modifiers` field
// already collected on `MethodDecl`/`PropertyDecl` (the identical shape the
// existing `static` check just above it uses), placed immediately after
// that `static` check.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =====================================================================
// REJECT — the 4 `VirtualKind` spellings (A.4: `"abstract" | "virtual" |
// "sealed"? "override"`) on an extension METHOD. Each asserts a real
// source location (never 0:0), the specific offending modifier's own text,
// and the §26.3 authority.
// =====================================================================

BEZEL_TEST(extension_virtual_kind_4228, extension_method_virtual_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetV4228a "
        "  public X: int; "
        "end WidgetV4228a; "
        "extension WidgetV4228a "
        "  public virtual method Vm4228a(): int => this.X; "
        "end extension; "
        "class CV4228a public static method Main(): int "
        "  var w := new WidgetV4228a(); "
        "  return w.Vm4228a(); "
        "end Main; end CV4228a;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension member may not be declared `virtual`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

BEZEL_TEST(extension_virtual_kind_4228, extension_method_abstract_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetV4228b "
        "  public X: int; "
        "end WidgetV4228b; "
        "extension WidgetV4228b "
        "  public abstract method Vm4228b(): int => this.X; "
        "end extension; "
        "class CV4228b public static method Main(): int "
        "  var w := new WidgetV4228b(); "
        "  return w.Vm4228b(); "
        "end Main; end CV4228b;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension member may not be declared `abstract`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

BEZEL_TEST(extension_virtual_kind_4228, extension_method_override_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetV4228c "
        "  public X: int; "
        "end WidgetV4228c; "
        "extension WidgetV4228c "
        "  public override method Vm4228c(): int => this.X; "
        "end extension; "
        "class CV4228c public static method Main(): int "
        "  var w := new WidgetV4228c(); "
        "  return w.Vm4228c(); "
        "end Main; end CV4228c;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension member may not be declared `override`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

BEZEL_TEST(extension_virtual_kind_4228,
           extension_method_sealed_override_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetV4228d "
        "  public X: int; "
        "end WidgetV4228d; "
        "extension WidgetV4228d "
        "  public sealed override method Vm4228d(): int => this.X; "
        "end extension; "
        "class CV4228d public static method Main(): int "
        "  var w := new WidgetV4228d(); "
        "  return w.Vm4228d(); "
        "end Main; end CV4228d;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // `modifiers` collects "sealed" before "override" (declaration order);
    // the check reports the FIRST VirtualKind spelling it finds — "sealed".
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension member may not be declared `sealed`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

// =====================================================================
// REJECT — the same 4 spellings on an extension EXPRESSION-BODIED PROPERTY
// (`ExtensionMember`'s other admitted alternative). §26.3's bullet names no
// distinction between methods and properties ("No `virtual`, `override`,
// `abstract`, or `sealed`"), so the same modifier check must fire on both.
// =====================================================================

BEZEL_TEST(extension_virtual_kind_4228,
           extension_property_virtual_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetV4228e "
        "  public X: int; "
        "end WidgetV4228e; "
        "extension WidgetV4228e "
        "  public virtual property Vp4228e: int => this.X; "
        "end extension; "
        "class CV4228e public static method Main(): int "
        "  var w := new WidgetV4228e(); "
        "  return w.Vp4228e; "
        "end Main; end CV4228e;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension member may not be declared `virtual`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

BEZEL_TEST(extension_virtual_kind_4228,
           extension_property_abstract_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetV4228f "
        "  public X: int; "
        "end WidgetV4228f; "
        "extension WidgetV4228f "
        "  public abstract property Vp4228f: int => this.X; "
        "end extension; "
        "class CV4228f public static method Main(): int "
        "  var w := new WidgetV4228f(); "
        "  return w.Vp4228f; "
        "end Main; end CV4228f;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension member may not be declared `abstract`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

BEZEL_TEST(extension_virtual_kind_4228,
           extension_property_override_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetV4228g "
        "  public X: int; "
        "end WidgetV4228g; "
        "extension WidgetV4228g "
        "  public override property Vp4228g: int => this.X; "
        "end extension; "
        "class CV4228g public static method Main(): int "
        "  var w := new WidgetV4228g(); "
        "  return w.Vp4228g; "
        "end Main; end CV4228g;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension member may not be declared `override`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

BEZEL_TEST(extension_virtual_kind_4228,
           extension_property_sealed_override_rejected_spec_26_3) {
    auto r = runAot(
        "module M; "
        "class WidgetV4228h "
        "  public X: int; "
        "end WidgetV4228h; "
        "extension WidgetV4228h "
        "  public sealed override property Vp4228h: int => this.X; "
        "end extension; "
        "class CV4228h public static method Main(): int "
        "  var w := new WidgetV4228h(); "
        "  return w.Vp4228h; "
        "end Main; end CV4228h;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "an extension member may not be declared `sealed`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§26.3"));
}

// =====================================================================
// ACCEPT — legitimate extension bodies and legitimate ordinary
// `virtual`/`override` class members must keep compiling (LANE_RULES §15 —
// a new rejection must not become a false reject of what §26.3, or ordinary
// class inheritance, admits).
// =====================================================================

// ACCEPT — an ordinary extension with a plain (non-VirtualKind) method and
// expression-bodied property: unaffected by this fix.
BEZEL_TEST(extension_virtual_kind_4228,
           extension_plain_method_and_property_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class WidgetOK4228 "
        "  public X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "end WidgetOK4228; "
        "extension WidgetOK4228 "
        "  public method DoIt4228(): int => this.X + 1; "
        "  public property Doubled4228: int => this.X * 2; "
        "end extension; "
        "class COK4228 public static method Main(): int "
        "  var w := new WidgetOK4228(20); "
        "  return w.DoIt4228() + w.Doubled4228; "
        "end Main; end COK4228;"),
        // DoIt4228() = 21, Doubled4228 = 40 -> 61
        61);
}

// ACCEPT — a REAL `virtual` base method overridden by a REAL `override` in a
// derived class (NOT inside any extension): this fix's new code lives only
// inside the `ExtensionDecl` arm, so ordinary class-hierarchy virtual
// dispatch must be entirely unaffected.
BEZEL_TEST(extension_virtual_kind_4228,
           ordinary_class_virtual_override_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class Base4228 "
        "  public virtual method Get4228(): int => 1; "
        "end Base4228; "
        "class Derived4228 : Base4228 "
        "  public override method Get4228(): int => 2; "
        "end Derived4228; "
        "class COrd4228 public static method Main(): int "
        "  var d := new Derived4228(); "
        "  return d.Get4228(); "
        "end Main; end COrd4228;"),
        2);
}
