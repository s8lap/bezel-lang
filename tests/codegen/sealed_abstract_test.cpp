// Codegen end-to-end for the §11.7 / §19.6 closed-abstract-hierarchy idiom.
// A `sealed abstract` base (a "closed abstract class") may be inherited from
// within its declaring module; the spec's recommended exhaustive-`case`
// pattern depends on this. Before the fix the frontend rejected every
// same-module subclass of a `sealed abstract` base ("cannot inherit from
// sealed class … §12.4"), making the idiom uncompilable (FINDING [6]).
//
// These tests JIT-run the §11.7 example form and assert the produced exit
// code, proving the program both compiles and runs.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// §11.7 / §19.6: `sealed abstract class Shape` with a same-module
// `sealed class Circle : Shape`. Circle(2).Area() = 3 * 2 * 2 = 12.
BEZEL_TEST(codegen_sealed_abstract, closed_hierarchy_same_module_runs_spec_11_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "sealed abstract class Shape "
        "  public abstract method Area(): int; "
        "end Shape; "
        "sealed class Circle : Shape "
        "  public R: int; "
        "  public constructor(r: int) R := r; end constructor; "
        "  public override method Area(): int => 3 * R * R; "
        "  destructor end destructor; "
        "end Circle; "
        "class C "
        "  public static method Main(): int "
        "    using var c := new Circle(2); "
        "    return c.Area(); "
        "  end Main; "
        "end C;"), 12);
}

// §11.7 / §19.6: dispatch through the `sealed abstract` base type. A
// Shape-typed local holding a Circle dispatches to Circle's override — the
// closed-hierarchy base is a usable polymorphic base, not merely declarable.
BEZEL_TEST(codegen_sealed_abstract, dispatch_through_closed_base_spec_19_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "sealed abstract class Shape "
        "  public abstract method Area(): int; "
        "end Shape; "
        "sealed class Square : Shape "
        "  public S: int; "
        "  public constructor(s: int) S := s; end constructor; "
        "  public override method Area(): int => S * S; "
        "  destructor end destructor; "
        "end Square; "
        "class C "
        "  public static method Main(): int "
        "    using var sq := new Square(4); "
        "    var sh: Shape := sq; "
        "    return sh.Area(); "
        "  end Main; "
        "end C;"), 16);
}
