// fix #5208 — method-reference formation must bridge a representation-changing
// §5.1 implicit conversion with a compiler-synthesized adapter (§27.6, v0.313).
//
// §27.6: "its parameter and return types unify with the target's under the
// standard implicit-conversion rules (§5.1) ... Formation may bridge
// representation-changing conversions by synthesizing an adapter at the
// reference site." §27.1.1 restates it: forming a reference from a NAMED
// method "may bridge any §5.1 implicit conversion, because the compiler
// synthesizes the adapter at the formation site".
//
// THE BUG. A reference whose target function type is satisfied only through a
// representation-changing conversion (`int -> double`, `int -> int?`, boxing)
// type-checked, but the thunk forwarded the caller's RAW argument bits, so the
// callee read a value of the wrong representation: `var g: (int) -> () := H.D`
// over `D(x: double)` printed `d:0`; `H.N(x: int?)` printed a garbage number.
//
// FAIL-WITHOUT: every value row below observes the wrong value (or crashes)
// on the pre-fix compiler.
//
// per §27.6 / §5.1 (issue #5208, spec v0.313).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

struct RunOut { bool ran; int exitCode; std::string stdoutText; };
RunOut runMod(const std::string& src) {
    JitResult r = runAot(src);
    return {r.ran, r.exitCode, r.stdoutText};
}

}  // namespace

// Issue probe 1 verbatim: `int -> double` through a method reference, beside an
// overloaded method that needs no bridge. Pre-fix: `int:3` then `d:0`.
BEZEL_TEST(codegen_method_ref_conversion_adapter_5208, issue_probe_int_to_double_spec_27_6) {
    auto r = runMod(
        "module M; "
        "class H "
        "  public static method On(x: string) Console.WriteLine($\"str:{x}\"); end On; "
        "  public static method On(x: int) Console.WriteLine($\"int:{x}\"); end On; "
        "  public static method D(x: double) Console.WriteLine($\"d:{x}\"); end D; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> () := H.On; "
        "    f(3); "
        "    var g: (int) -> () := H.D; "
        "    g(4); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "int:3\nd:4\n");
}

// Issue probe 2 verbatim: `long` widening (already worked), boxing `int ->
// Object`, and `int -> int?`. Pre-fix the third line was garbage
// (`n:1272168470`, varies).
BEZEL_TEST(codegen_method_ref_conversion_adapter_5208, issue_probe_long_object_nullable_spec_27_6) {
    auto r = runMod(
        "module M; "
        "class H "
        "  public static method L(x: long): long return x * 2; end L; "
        "  public static method O(x: Object) Console.WriteLine(\"obj\"); end O; "
        "  public static method N(x: int?) Console.WriteLine($\"n:{x}\"); end N; "
        "  public static method R(x: int): long return x + 1; end R; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var a: (int) -> long := H.L; "
        "    Console.WriteLine($\"{a(4)}\"); "
        "    var b: (int) -> () := H.O; "
        "    b(4); "
        "    var c: (int) -> () := H.N; "
        "    c(4); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "8\nobj\nn:4\n");
}

// Nullable slots observed by value: `int -> long?` (S -> T -> T?), the lifted
// `int? -> long?` with a present value AND with null (null must stay null),
// and `int -> decimal`.
BEZEL_TEST(codegen_method_ref_conversion_adapter_5208, nullable_lift_and_decimal_spec_5_1) {
    auto r = runMod(
        "module M; "
        "class H "
        "  public static method LN(x: long?): long? return x; end LN; "
        "  public static method DecM(x: decimal): decimal return x + 1.5m; end DecM; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var b: (int) -> long? := H.LN; "
        "    Console.WriteLine($\"{b(7)}\"); "
        "    var c: (int?) -> long? := H.LN; "
        "    Console.WriteLine($\"{c(9)} {c(null)}\"); "
        "    var d: (int) -> decimal := H.DecM; "
        "    Console.WriteLine($\"{d(2)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n9 null\n3.5\n");
}

// Unsigned source: `uint -> long` must zero-extend (4000000000 is negative as
// a signed i32), `byte -> double` likewise.
BEZEL_TEST(codegen_method_ref_conversion_adapter_5208, unsigned_widening_spec_5_1) {
    auto r = runMod(
        "module M; "
        "class H "
        "  public static method U(x: long): long return x; end U; "
        "  public static method B(x: double): double return x; end B; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var u: (uint) -> long := H.U; "
        "    Console.WriteLine($\"{u(uint(4000000000L))}\"); "
        "    var b: (byte) -> double := H.B; "
        "    Console.WriteLine($\"{b(byte(200))}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4000000000\n200\n");
}

// Return direction (§27.6: return types unify covariantly under §5.1):
// callee `int` bridged to a `double` and to an `int?` return slot.
BEZEL_TEST(codegen_method_ref_conversion_adapter_5208, return_slot_widening_and_wrapping_spec_27_6) {
    auto r = runMod(
        "module M; "
        "class H "
        "  public static method RI(x: int): int return x + 1; end RI; "
        "  public static method Ret(x: int): int return x * 3; end Ret; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var g: (int) -> double := H.RI; "
        "    Console.WriteLine($\"{g(5) / 4.0}\"); "
        "    var h: (int) -> int? := H.Ret; "
        "    Console.WriteLine($\"{h(5)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1.5\n15\n");
}

// The bridge is applied at every formation site, not only a `var` with a
// declared type: a return statement, a call argument (single-overload AND
// overloaded callee, where overload resolution never ran checkAssignable), a
// field initializer, an assignment and a record positional argument.
BEZEL_TEST(codegen_method_ref_conversion_adapter_5208, every_formation_site_spec_27_6) {
    auto r = runMod(
        "module M; "
        "record Rec(Cb: (int) -> double); "
        "class H "
        "  public static method D(x: double): double return x * 2.0; end D; "
        "  public static method Pr(x: double) Console.WriteLine($\"p:{x}\"); end Pr; "
        "end H; "
        "class Box "
        "  public F: (int) -> double := H.D; "
        "end Box; "
        "class C "
        "  public static method Mk(): (int) -> double "
        "    return H.D; "
        "  end Mk; "
        "  public static method Ap(f: (int) -> double, v: int): double return f(v); end Ap; "
        "  public static method Take(f: (long) -> ()) Console.WriteLine(\"L\"); end Take; "
        "  public static method Take(f: (int) -> (), n: int) f(n); end Take; "
        "  public static method Main(): int "
        "    Console.WriteLine($\"{Mk()(6)}\"); "
        "    Console.WriteLine($\"{Ap(H.D, 8)}\"); "
        "    Take(H.Pr, 5); "
        "    var b := new Box(); "
        "    Console.WriteLine($\"{b.F(1)}\"); "
        "    var r := new Rec(H.D); "
        "    Console.WriteLine($\"{r.Cb(3)}\"); "
        "    var t: (int) -> double; "
        "    t := H.D; "
        "    Console.WriteLine($\"{t(6)}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "12\n16\np:5\n2\n6\n12\n");
}

// Boxing bridge (`int -> Object`): the callee OBSERVES the boxed value (the
// raw-bits thunk handed it an int where a box pointer was expected), and a
// delegate called 200 times must not leak the per-call box (released after the
// call when the callee provably does not retain the parameter, #2952's rule).
// runAotChecked = generated-code ASan/LSan, so a leaked box fails the row.
BEZEL_TEST(codegen_method_ref_conversion_adapter_5208, boxing_bridge_observes_value_no_leak_loop_spec_5_1) {
    auto r = runAotChecked(
        "module M; "
        "class H "
        "  public static method Obj(x: Object): int "
        "    if x is int then return 1; end if; "
        "    return 0; "
        "  end Obj; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var e: (int) -> int := H.Obj; "
        "    var sum := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      sum := sum + e(i); "
        "      i := i + 1; "
        "    end while; "
        "    if sum = 200 then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!r.ran && !r.diagnostics.diagnostics().empty() &&
        r.diagnostics.diagnostics()[0].message.rfind("runAotChecked: SKIP", 0) == 0)
        return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// Control: a reference that needs NO conversion is unchanged (and keeps its
// `event -= H.M` identity thunk: same signature, same cached thunk).
BEZEL_TEST(codegen_method_ref_conversion_adapter_5208, control_no_conversion_needed_spec_27_6) {
    auto r = runMod(
        "module M; "
        "class H "
        "  public static method Sq(x: int): int return x * x; end Sq; "
        "  public static method Up(x: string): string return x + \"!\"; end Up; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> int := H.Sq; "
        "    var g: (string) -> string := H.Up; "
        "    Console.WriteLine($\"{f(7)} {g(\"hi\")}\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "49 hi!\n");
}

// Residual that this implementation does not bridge: a RETURN slot needing a
// boxing conversion (`int -> Object`; the minted box's ownership at the
// caller is unspecified for an adapter). It must be a located compile error,
// never a silently mis-represented value.
BEZEL_TEST(codegen_method_ref_conversion_adapter_5208, return_slot_boxing_is_rejected_not_miscompiled) {
    auto r = runAot(
        "module M; "
        "class H "
        "  public static method RI(x: int): int return x + 1; end RI; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var g: (int) -> Object := H.RI; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool cited = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("fix #5208") != std::string::npos) cited = true;
    BEZEL_EXPECT_TRUE(cited);
}
