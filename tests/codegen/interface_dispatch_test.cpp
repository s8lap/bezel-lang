// Codegen slice CG35 — interface method dispatch (§11.2). Each interface
// method gets a fixed vtable slot shared across all implementers; a call
// through an interface-typed value indexes the runtime object's vtable at
// that slot, so the implementing class's method runs. Observable via the
// int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg35, call_via_interface_typed_parameter) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Square : IShape public method Area(): int => 16; end Square; "
        "class C "
        "  public static method AreaOf(s: IShape): int => s.Area(); "
        "  public static method Main(): int "
        "    var sq := new Square(); return AreaOf(sq); "
        "  end Main; "
        "end C;"), 16);
}

BEZEL_TEST(codegen_cg35, polymorphism_two_implementers) {
    // Two classes implement IShape differently; calling through the
    // interface dispatches to each one's method.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Square : IShape public method Area(): int => 4; end Square; "
        "class Big : IShape public method Area(): int => 100; end Big; "
        "class C "
        "  public static method AreaOf(s: IShape): int => s.Area(); "
        "  public static method Main(): int "
        "    var a := new Square(); var b := new Big(); "
        "    return AreaOf(a) + AreaOf(b); "
        "  end Main; "
        "end C;"), 104);
}

BEZEL_TEST(codegen_cg35, call_via_interface_local) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Square : IShape public method Area(): int => 25; end Square; "
        "class C public static method Main(): int "
        "  var s: IShape := new Square(); return s.Area(); "
        "end Main; end C;"), 25);
}

BEZEL_TEST(codegen_cg35, interface_with_argument) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IAdder method Add(n: int): int; end IAdder; "
        "class Plus10 : IAdder "
        "  public method Add(n: int): int => n + 10; end Plus10; "
        "class C "
        "  public static method Use(a: IAdder): int => a.Add(32); "
        "  public static method Main(): int "
        "    var p := new Plus10(); return Use(p); "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg35, two_interface_methods) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IPair method First(): int; method Second(): int; "
        "end IPair; "
        "class P : IPair "
        "  public method First(): int => 40; "
        "  public method Second(): int => 2; "
        "end P; "
        "class C "
        "  public static method Sum(p: IPair): int => p.First() + p.Second(); "
        "  public static method Main(): int "
        "    var p := new P(); return Sum(p); "
        "  end Main; "
        "end C;"), 42);
}
