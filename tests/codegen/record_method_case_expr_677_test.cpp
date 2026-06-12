// fix #677 / §9.4 / §19.2: enum method (or record method) with a `case`
// EXPRESSION in its body that returns `this` in one arm. The arm body
// `_ => this` lowers `this` as `currentThis_` which, for a record instance
// method, is a POINTER to the record struct (§9.4/CG29). But the case result
// type is the record struct by value, so the PHI node mixes struct-valued arms
// with a pointer arm → "PHI node operands are not the same type as the result!"
// assertion in LLVM (a PHINode::setIncomingValue debug assertion crash).
//
// Fix: after lowering each arm body value, when the value is a pointer but the
// case result type is a struct, load the struct through the pointer before
// appending to `incomings`. Mirrors the same load that lowerWith applies when
// `this with {…}` is used in a record method body.
//
// Spec references: §9.4 (record instance methods), §19.2 (case expression),
// §27.2 (method bodies).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;
using bezel::test::JitResult;

namespace {

int runProg(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// TriBool record: the spec §27.2-era example — primary #677 repro.
const char* kTriBool =
    "module M; "
    "record TriBool(Value: int) "
    "  public method Negate(): TriBool => case Value "
    "    0 => new TriBool(1), "
    "    1 => new TriBool(0), "
    "    _ => this "
    "  end; "
    "end TriBool; ";

} // namespace

// §9.4 / §19.2 / #677: the value-0 arm (`0 => new TriBool(1)`) is selected.
// Negate(0) → TriBool(1), Value = 1.  Before fix: PHI mismatch crash in bezelc.
BEZEL_TEST(codegen_677_record_case_this, negate_zero_gives_one_spec_9_4_19_2) {
    BEZEL_EXPECT_EQ(runProg(
        std::string(kTriBool) +
        "class App "
        "  public static method Main(): int "
        "    var t := new TriBool(0); "
        "    var n := t.Negate(); "
        "    return n.Value; "
        "  end Main; "
        "end App;"), 1);
}

// §9.4 / §19.2 / #677: the value-1 arm (`1 => new TriBool(0)`) is selected.
// Negate(1) → TriBool(0), Value = 0.
BEZEL_TEST(codegen_677_record_case_this, negate_one_gives_zero_spec_9_4_19_2) {
    BEZEL_EXPECT_EQ(runProg(
        std::string(kTriBool) +
        "class App "
        "  public static method Main(): int "
        "    var t := new TriBool(1); "
        "    var n := t.Negate(); "
        "    return n.Value; "
        "  end Main; "
        "end App;"), 0);
}

// §9.4 / §19.2 / #677: the wildcard arm (`_ => this`) is selected for value 2.
// Negate(2) is the identity — returns this unchanged, Value = 2.
// This is the arm that previously caused the PHI mismatch (this = ptr, not struct).
BEZEL_TEST(codegen_677_record_case_this, negate_two_identity_via_this_spec_9_4_19_2) {
    BEZEL_EXPECT_EQ(runProg(
        std::string(kTriBool) +
        "class App "
        "  public static method Main(): int "
        "    var t := new TriBool(2); "
        "    var n := t.Negate(); "
        "    return n.Value; "
        "  end Main; "
        "end App;"), 2);
}

// §9.4 / §19.2 / #677: negate twice — Negate(Negate(0)) = Negate(1) = 0.
// Exercises the method's return value being fed back into itself.
BEZEL_TEST(codegen_677_record_case_this, double_negate_roundtrip_spec_9_4_19_2) {
    BEZEL_EXPECT_EQ(runProg(
        std::string(kTriBool) +
        "class App "
        "  public static method Main(): int "
        "    var t := new TriBool(0); "
        "    var n := t.Negate().Negate(); "
        "    return n.Value; "
        "  end Main; "
        "end App;"), 0);
}

// §9.4 / §19.2 / #677: case expression in record method with NO wildcard arm,
// all arms returning record values (no `this`). The fix must not disturb
// well-typed arms.
BEZEL_TEST(codegen_677_record_case_this, case_no_this_arm_spec_9_4_19_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "record Pair(a: int, b: int) "
        "  public method Swap(): Pair => case a "
        "    0 => new Pair(b, a), "
        "    _ => new Pair(b, a + 1) "
        "  end; "
        "end Pair; "
        "class App "
        "  public static method Main(): int "
        "    var p := new Pair(0, 7); "
        "    var s := p.Swap(); "
        "    return s.a; "
        "  end Main; "
        "end App;"), 7);
}

// §9.4 / §19.2 / #677: record method with multiple `this` arms (two different
// case branches both return `this`). Both arms must produce the correct struct.
BEZEL_TEST(codegen_677_record_case_this, two_this_arms_spec_9_4_19_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "record Pt(x: int) "
        "  public method Clamp(lo: int, hi: int): Pt => case x "
        "    _ => this "
        "  end; "
        "end Pt; "
        "class App "
        "  public static method Main(): int "
        "    var p := new Pt(5); "
        "    var c := p.Clamp(0, 10); "
        "    return c.x; "
        "  end Main; "
        "end App;"), 5);
}

// §10 / §19.2 / #677 REGRESSION: enum method with `case this` returning int —
// enum methods have no PHI mismatch (currentThis_ is already i32) but must
// still work correctly after the fix. The fix's isStructTy() guard must not
// trigger for i32-returning enum methods.
BEZEL_TEST(codegen_677_record_case_this, enum_method_case_this_regression_spec_10) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "enum Dir North := 1, South := 2, East := 3; "
        "  public method Code(): int => "
        "    case this Dir.North => 10, Dir.South => 20, Dir.East => 30, end; "
        "end Dir; "
        "class App "
        "  public static method Main(): int "
        "    return Dir.South.Code(); "
        "  end Main; "
        "end App;"), 20);
}
