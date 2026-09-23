// Spec-derived tests for the §11.7 static-class inheritance and interface-
// implementation rejection (cluster s11_7, baseline rows s11_7_0004 and
// s11_7_0013).
//
// §11.7 ("Static classes", v0.307), verbatim:
//   "A static class may not be inherited from and does not implement
//    interfaces."
// The grammar (Appendix A.3, A.4) over-admits any `Modifier*` set and any
// `:`-list of base types on a class declaration, so the rejection is a
// SEMANTIC responsibility — the parser hands `static class Utils: Base`
// and `static class Utils: IMarker` to sema, which must reject them.
//
// MEASURED on the pre-fix `572f5fc5` baseline: both inputs compile to a
// runnable executable (verified by `build/bezelc -o out … && ls out`,
// which produces a 320 KiB native binary and `runAot` reports `ran=true,
// exit=5` for the inheritance form, `ran=true, exit=2` for the interface
// form). That is the §11.7 divergence the cluster recorded as `fail` in
// `tests/conformance/rules/baseline.tsv`.
//
// `Resolver::checkStaticClassInheritance` (TypeResolution_Checks.cpp) is
// the fix. It runs after type resolution populates `t->baseTypes` (so
// `checkStaticClassInheritance`'s read of `t->baseTypes[bi]` is the
// resolved TypeSymbol — the same shape every other §11.7-shaped check
// uses) and produces TWO spec-cited diagnostics, one per kind of base:
//   - CLASS base   → "static class `X` may not be inherited from `B`"
//   - INTERFACE    → "static class `X` may not implement interface `I`"
// `diagnostic: static` is the substring each case's authored
// `//! diagnostic:` field asserts the compile log carries; both messages
// above satisfy it (every relevant English noun — "static class",
// "static", "interface" — appears in the message itself).
//
// Following the sibling #4186 test's discipline (LANE_RULES §15: "a fix
// that adds a new rejection must not become a false reject of what the
// spec actually admits"), the ACCEPT arm of this test pins the four
// shapes §11.7 still admits:
//   1. A static class with NO base list (the spec's worked example).
//   2. An ordinary class extending another class (inheritance is fine).
//   3. An ordinary class implementing an interface (conformance is fine).
//   4. A nested static class — the §11.6 / #3574 nested-type walker
//      must reach it (else the rule silently retreats at depth 1).
// A fix that over-rejects any of these would regress legitimate code and
// would not be acceptable even though the bug it fixes is real.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

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
// REJECT — §11.7 verbatim: a static class may not be inherited from
// another class. This is the issue's repro for s11_7_0004.
// =====================================================================

BEZEL_TEST(static_class_inheritance_and_interfaces_4717,
           static_class_extending_class_rejected_spec_11_7) {
    auto r = runAot(
        "module M; "
        "public class Base4717 "
        "  public constructor() "
        "  end constructor; "
        "end Base4717; "
        "public static class Utils4717: Base4717 "
        "  public static method Max(a: int, b: int): int => "
        "    (if a > b then a else b); "
        "end Utils4717; "
        "class C4717 public static method Main(): int "
        "  return 0; "
        "end Main; end C4717;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r,
        "static class `Utils4717` may not be inherited from `Base4717`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§11.7"));
}

// =====================================================================
// REJECT — §11.7 verbatim: a static class does not implement interfaces.
// This is the issue's repro for s11_7_0013.
// =====================================================================

BEZEL_TEST(static_class_inheritance_and_interfaces_4717,
           static_class_implementing_interface_rejected_spec_11_7) {
    auto r = runAot(
        "module M; "
        "public interface IMarker4717 "
        "end IMarker4717; "
        "public static class Utils4717b: IMarker4717 "
        "  public static method Max(a: int, b: int): int => "
        "    (if a > b then a else b); "
        "end Utils4717b; "
        "class C4717b public static method Main(): int "
        "  return 0; "
        "end Main; end C4717b;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r,
        "static class `Utils4717b` may not implement interface "
        "`IMarker4717`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§11.7"));
}

// REJECT — both arms together (a base class AND an interface in the same
// inheritance list). The check walks the full base list per static class,
// so the diagnostic must fire for BOTH entries, not just the first.
BEZEL_TEST(static_class_inheritance_and_interfaces_4717,
           static_class_with_class_and_interface_bases_rejected_spec_11_7) {
    auto r = runAot(
        "module M; "
        "public class Base4717c "
        "  public constructor() end constructor; "
        "end Base4717c; "
        "public interface IMarker4717c "
        "end IMarker4717c; "
        "public static class Utils4717c: Base4717c, IMarker4717c "
        "  public static method M(): int => 0; "
        "end Utils4717c; "
        "class C4717c public static method Main(): int "
        "  return 0; "
        "end Main; end C4717c;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "may not be inherited from `Base4717c`"));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "may not implement interface `IMarker4717c`"));
}

// REJECT — a NESTED static class (§11.6 / #3574) extending a base class.
// The §11.7 rule applies equally to nested types; the recursive
// `forEachTypeAndNested` walk must reach depth >= 1 or the rule retreats
// to top-level only.
BEZEL_TEST(static_class_inheritance_and_interfaces_4717,
           nested_static_class_extending_class_rejected_spec_11_7) {
    auto r = runAot(
        "module M; "
        "public class Outer4717 "
        "  public class InnerBase "
        "    public constructor() end constructor; "
        "  end InnerBase; "
        "  public static class Inner4717: InnerBase "
        "    public static method M(): int => 0; "
        "  end Inner4717; "
        "end Outer4717; "
        "class C4717d public static method Main(): int "
        "  return 0; "
        "end Main; end C4717d;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r,
        "static class `Inner4717` may not be inherited from `InnerBase`"));
}

// =====================================================================
// ACCEPT — control cases. §11.7 still admits every non-conflicting
// class shape; a fix that over-rejects any of these would regress
// legitimate code.
// =====================================================================

// ACCEPT — §11.7's own worked example: a static class with NO base list,
// containing only a static method.
BEZEL_TEST(static_class_inheritance_and_interfaces_4717,
           static_class_without_bases_accepted_control_spec_11_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public static class Utils4717e "
        "  public static method Max(a: int, b: int): int => "
        "    (if a > b then a else b); "
        "end Utils4717e; "
        "class C4717e public static method Main(): int "
        "  return Utils4717e.Max(2, 42); "
        "end Main; end C4717e;"),
        42);
}

// ACCEPT — an ordinary (non-static) class extending another ordinary class.
// Inheritance is permitted on non-static classes; only the static shape is
// excluded by this rule. `virtual` on the base method is what lets the
// override dispatch (`override` on its own is not a class-level switch).
BEZEL_TEST(static_class_inheritance_and_interfaces_4717,
           ordinary_class_extending_class_accepted_control_spec_11) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Animal4717 "
        "  public constructor() end constructor; "
        "  public virtual method Speak(): int => 0; "
        "end Animal4717; "
        "public class Dog4717: Animal4717 "
        "  public constructor() end constructor; "
        "  public override method Speak(): int => 42; "
        "end Dog4717; "
        "class C4717f public static method Main(): int "
        "  var a: Animal4717 := new Dog4717(); "
        "  return a.Speak(); "
        "end Main; end C4717f;"),
        42);
}

// ACCEPT — an ordinary class implementing an interface (the §11.2
// conformance rule). The check is restricted to `static class` types, so
// a normal conformance must compile and run.
BEZEL_TEST(static_class_inheritance_and_interfaces_4717,
           ordinary_class_implementing_interface_accepted_control_spec_11_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public interface IShape4717 "
        "  method Area(): int; "
        "end IShape4717; "
        "public class Square4717: IShape4717 "
        "  public constructor() end constructor; "
        "  public method Area(): int => 42; "
        "end Square4717; "
        "class C4717g public static method Main(): int "
        "  var s: IShape4717 := new Square4717(); "
        "  return s.Area(); "
        "end Main; end C4717g;"),
        42);
}

// ACCEPT — a NESTED static class (§11.6 / #3574) with NO base list.
// Symmetric to the rejection case above: the recursive walk reaches the
// nested type and correctly finds no violation.
BEZEL_TEST(static_class_inheritance_and_interfaces_4717,
           nested_static_class_without_bases_accepted_control_spec_11_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Outer4717h "
        "  public static class Inner4717h "
        "    public static method Answer(): int => 42; "
        "  end Inner4717h; "
        "end Outer4717h; "
        "class C4717h public static method Main(): int "
        "  return Outer4717h.Inner4717h.Answer(); "
        "end Main; end C4717h;"),
        42);
}
