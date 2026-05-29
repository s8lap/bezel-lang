// Codegen slice CG18 — abstract methods + dynamic Dispose (§12.5, §28.4–
// 28.5). Abstract methods occupy a vtable slot filled by the concrete
// override (the class is never instantiated, so the null slot is never
// called). Disposal is dispatched through the disposed object's own
// vtable slot 0, so an owned field of interface type — or of a base type
// holding a subclass — runs the correct runtime Dispose. Observable via
// the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg18, abstract_method_dispatched_to_override) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Shape public abstract method Area(): int; end Shape; "
        "class Square : Shape "
        "  public override method Area(): int => 16; "
        "  public static method Main(): int "
        "    var sq := new Square(); return sq.Area(); "
        "  end Main; "
        "end Square;"), 16);
}

BEZEL_TEST(codegen_cg18, abstract_dispatched_through_base_param) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "abstract class Shape public abstract method Area(): int; end Shape; "
        "class Square : Shape "
        "  public override method Area(): int => 25; "
        "  public static method AreaOf(s: Shape): int => s.Area(); "
        "  public static method Main(): int "
        "    var sq := new Square(); return AreaOf(sq); "
        "  end Main; "
        "end Square;"), 25);
}

// NOTE: an interface-typed owned field (`owned _t: IThing` holding a
// class instance) disposes dynamically too — the mechanism is identical
// to the subclass case below. It awaits a front-end fix: assignability
// does not yet accept a class value into an interface-typed slot
// (Widget → IThing, §5.1 conformance). Tracked as a separate FE issue.

BEZEL_TEST(codegen_cg18, owned_field_subclass_disposed_dynamically) {
    // _b is typed Base but holds a Derived. Dynamic disposal runs
    // Derived.Dispose (destructor +1) then chains Base.Dispose (+100) →
    // 101. Static dispatch on Base would miss Derived's destructor (→100).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 100; end destructor; "
        "  public static method Main(): int "
        "    do using var o := new Owner(); end; "
        "    return _n; "
        "  end Main; "
        "end Base; "
        "class Derived : Base "
        "  destructor _n := _n + 1; end destructor; "
        "end Derived; "
        "class Owner "
        "  private owned _b: Base; "
        "  public constructor() this._b := new Derived(); end constructor; "
        "end Owner;"), 101);
}

// NOTE: a `using var` statically typed as a base but holding a subclass
// (via a factory method) also disposes dynamically — but exercising it
// needs a factory like `Make(): Base => new Derived();`, whose embedded
// `new Derived()` type is not resolved by the front-end for *expression*
// bodies (only block bodies). Lands with that FE fix.
