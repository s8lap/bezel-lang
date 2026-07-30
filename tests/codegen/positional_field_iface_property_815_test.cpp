// fix #815 / §9.1 / §14.2 / §13: a POSITIONAL record field generates a public
// read-only property of the same name (§9.1), which can satisfy an interface
// property. But the field is a FieldSymbol (with a non-null posParam), not a
// PropertySymbol, so codegen's property-by-name lookup missed it: the interface
// itable slot was a null function pointer (SIGSEGV on dispatch through an
// interface-typed variable) and the generic-constraint devirtualization fell
// through to an UndefValue (garbage result). The fix recognizes a positional
// field as the property implementation and emits a struct-GEP+load getter at
// both the itable-build and the constraint-dispatch sites.
//
// #2416 (§9.1/§9.4/§11.2/§23.3, v0.235): every interface property below is
// declared `readonly`. Before #2416, checkAbstractImplementations skipped
// records entirely, so a getter-only positional-field property silently
// satisfied a nominally READ-WRITE interface property obligation — the exact
// null-itable-setter-slot SIGSEGV shape #2413 fixed for classes, just never
// checked for records. None of these tests ever write through the interface
// reference (only `ipt.A` / `ip.X` / `ip.Y` reads), so per #2413's own
// remediation precedent the intent-preserving fix is to mark the obligation
// `readonly` — a positional field's auto-generated property can never
// provide a setter (§9.4: "Properties auto-generated from positional fields
// are read-only"), so `readonly` is not merely intent-preserving here, it is
// the ONLY accessor set a positional field can ever satisfy.
//
// Value (dispatch) tests — runJit returns the raw int.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §9.1 / §13: a positional field accessed through an INTERFACE-typed variable.
// 42 (was a SIGSEGV — null itable slot).
BEZEL_TEST(positional_field_iface_property_815, via_interface_variable) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IFoo readonly property A: int; end IFoo; "
        "record FooPoint(A: int) : IFoo end FooPoint; "
        "class C public static method Main(): int "
        "  var pt := new FooPoint(42); "
        "  var ipt: IFoo := pt; "
        "  return ipt.A; "                              // 42
        "end Main; end C;"), 42);
}

// §9.1 / §14.2: a positional field accessed through a generic constraint
// (`where T: IFoo`). 42 (was 144 — UndefValue garbage).
BEZEL_TEST(positional_field_iface_property_815, via_generic_constraint) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IFoo readonly property A: int; end IFoo; "
        "record FooPoint(A: int) : IFoo end FooPoint; "
        "static class G "
        "  public static method Get<T>(p: T): int where T: IFoo => p.A; "
        "end G; "
        "class C public static method Main(): int "
        "  return G.Get(new FooPoint(42)); "            // 42
        "end Main; end C;"), 42);
}

// §9.1: two positional fields, both reachable through the interface itable.
// 2*10 + 3 = 23.
BEZEL_TEST(positional_field_iface_property_815, two_positional_fields_via_interface) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IPt readonly property X: int; readonly property Y: int; end IPt; "
        "record P(X: int, Y: int) : IPt end P; "
        "class C public static method Main(): int "
        "  var p := new P(2, 3); "
        "  var ip: IPt := p; "
        "  return ip.X * 10 + ip.Y; "                   // 23
        "end Main; end C;"), 23);
}

// Regression guard: an EXPLICIT property implementation (a PropertySymbol, the
// path that already worked) is unaffected. 42.
BEZEL_TEST(positional_field_iface_property_815, explicit_property_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IFoo readonly property A: int; end IFoo; "
        "record FooPoint(v: int) : IFoo "
        "  public property A: int => v; "
        "end FooPoint; "
        "class C public static method Main(): int "
        "  var pt := new FooPoint(42); "
        "  var ipt: IFoo := pt; "
        "  return ipt.A; "                              // 42
        "end Main; end C;"), 42);
}
