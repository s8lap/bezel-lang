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
// Value (dispatch) tests — runJit returns the raw int.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §9.1 / §13: a positional field accessed through an INTERFACE-typed variable.
// 42 (was a SIGSEGV — null itable slot).
BEZEL_TEST(positional_field_iface_property_815, via_interface_variable) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IFoo property A: int; end IFoo; "
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
        "interface IFoo property A: int; end IFoo; "
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
        "interface IPt property X: int; property Y: int; end IPt; "
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
        "interface IFoo property A: int; end IFoo; "
        "record FooPoint(v: int) : IFoo "
        "  public property A: int => v; "
        "end FooPoint; "
        "class C public static method Main(): int "
        "  var pt := new FooPoint(42); "
        "  var ipt: IFoo := pt; "
        "  return ipt.A; "                              // 42
        "end Main; end C;"), 42);
}
