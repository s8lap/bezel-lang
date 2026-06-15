// Codegen slice CG49 — block-form property setters (§22.9). A property with
// a `set => do … end` (or `set => expr`) accessor runs that body on
// `obj.P := v`, with the implicit `value` parameter (renamable via
// `set(name)`) bound to the assigned value. The getter side (CG33c) and
// auto-properties are unchanged. Observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

BEZEL_TEST(codegen_cg49, block_setter_stores_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int get => _x; set => do _x := value; end; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 42; return c.P; "
        "  end Main; "
        "end C;"), 42);
}

BEZEL_TEST(codegen_cg49, setter_transforms_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int "
        "    get => _x; set => do _x := value * 2; end; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 5; return c.P; "
        "  end Main; "
        "end C;"), 10);
}

BEZEL_TEST(codegen_cg49, renamed_setter_parameter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int "
        "    get => _x; set(v) => do _x := v + 1; end; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 7; return c.P; "
        "  end Main; "
        "end C;"), 8);
}

BEZEL_TEST(codegen_cg49, setter_via_implicit_this) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int get => _x; set => do _x := value; end; end; "
        "  public method Init() P := 33; end Init; "
        "  public static method Main(): int "
        "    var c := new C(); c.Init(); return c.P; "
        "  end Main; "
        "end C;"), 33);
}

BEZEL_TEST(codegen_cg49, auto_property_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public property V: int; "
        "  public static method Main(): int "
        "    var c := new C(); c.V := 99; return c.V; "
        "  end Main; "
        "end C;"), 99);
}

// §22.9 (issue #248): compound assignment on a block-form property is a
// read-modify-write through the getter and setter, not a silent no-op.
BEZEL_TEST(codegen_cg49, compound_assign_runs_getter_and_setter) {
    // Doubling setter: `:= 10` stores _x=20; `+= 5` reads 20, +5 = 25, the
    // setter doubles → _x = 50.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int "
        "    get => _x; set => do _x := value * 2; end; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 10; c.P += 5; return c.P; "
        "  end Main; "
        "end C;"), 50);
}

BEZEL_TEST(codegen_cg49, compound_assign_identity_setter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int get => _x; set => do _x := value; end; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 20; c.P -= 7; return c.P; "
        "  end Main; "
        "end C;"), 13);
}

BEZEL_TEST(codegen_cg49, compound_assign_times_equals) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int get => _x; set => do _x := value; end; end; "
        "  public static method Main(): int "
        "    var c := new C(); c.P := 6; c.P *= 4; return c.P; "
        "  end Main; "
        "end C;"), 24);
}

// #739 / §22.9: a `return;` in a block-form setter ENDS the setter (it is a void
// accessor), it does NOT return from the enclosing method. The setter body is
// inlined at the assignment site, so the return is routed to an accessor-end
// block. Conditional-return validate-then-set must keep the prior value.
BEZEL_TEST(codegen_block_setter, conditional_return_ends_setter_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box "
        "  public _x: int := 0; "
        "  public property X: int "
        "    get => _x; "
        "    set => do if value < 0 then return; end; _x := value; end; "
        "  end X; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(); "
        "  b.X := 5; b.X := -1; b.X := 8; "  // -1 rejected; X = 8
        "  return b.X; "
        "end Main; end C;"), 8);
}

// #739 / §22.9: an UNCONDITIONAL `return;` in a setter previously IR-failed
// (terminator mid-block). Now it cleanly ends the setter (a no-op setter here).
BEZEL_TEST(codegen_block_setter, unconditional_return_in_setter_no_irfail_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box "
        "  public _x: int := 7; "
        "  public property X: int get => _x; set => do return; end; end X; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box(); b.X := 99; "  // setter is a no-op
        "  return b.X; "  // still 7
        "end Main; end C;"), 7);
}
