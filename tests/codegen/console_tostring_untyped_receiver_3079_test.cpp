// fix #3079 — `Console.WriteLine(x.ToString())` silently emitted NOTHING for
// two receiver shapes whose sema type was never recorded.
//
// §22.12 / §15.1.2 / §8.5 / §2.7, v0.249.
//
// §22.12, verbatim: "A class-level `const` is implicitly static"; "`const`
// declarations (with or without `static`) require a compile-time-constant
// expression (§17.14 — the referenced definition) and a primitive value type or
// `string`". §2.7 rule 1 reaches such a member unqualified from inside the
// declaring type (the chain-head member fallback), so `K` and `C.K` name the
// same constant and must behave identically.
//
// §15.1.2, verbatim: "`arr.Length: int` — compile-time constant N."
//
// §8.5 makes `ToString` universal; stdlib §9.1 declares `Int32.ToString():
// string` (decimal rendering). Neither shape is touched by v0.241's #2934/#2661
// amendment, which re-pinned only the CLASS / enum / non-auto-equality
// body-record qualified-name defaults — both receivers here are `int`.
//
// THE DEFECT was sema-side and identical in kind at both sites: the expression
// carried NO exprType. Only the qualified `C.K` spelling was typed (#609's
// `ConstMemberSymbol` arm in resolveMemberAccess); the bare-identifier
// type-attachment chain had no `ConstMemberSymbol` arm at all. And the
// §15.1.2 inline-array member block set an exprType for `ToArray`, `Contains`,
// and `ToString` but not for the `Length` sibling on the same allow-list.
//
// An untyped RECEIVER is not inert: CG27c (LlvmBackend_Exprs.cpp, "ToString()
// on any primitive") is gated on `ctx_.exprType(*ma->target)` being a
// Primitive, so the untyped receiver skipped every primitive-method arm and
// `lowerCall`'s `if (!ms)` chain fell through to its unconditional
// `return nullptr` — which `tryLowerConsoleCall` read as "not a console call",
// deleting the ENTIRE statement. MEASURED on 309356e: exit 0, zero diagnostics
// at any severity, and not even the line terminator on stdout.
//
// The tests assert the FULL stdout rather than an exit code, because the
// reported symptom IS missing output: a `<`/`>` frame around each shape makes
// a dropped line fail as a wrong string instead of passing as a wrong-but-
// plausible number.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// Shape (i): a BARE reference to a class-level `const` as the ToString
// receiver. The qualified `C.K.ToString()` control in the same program is the
// pre-empt: it worked before this fix (#609 typed it), so the two spellings
// agreeing is what proves the bare path was the gap.
BEZEL_TEST(codegen_console_tostring_untyped_receiver_3079,
           bare_class_const_tostring_prints_spec_22_12_2_7_8_5) {
    auto r = runAot(
        "module N3079A; "
        "class C "
        "  public const K: int := 7; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"<\"); "
        "    Console.WriteLine(C.K.ToString()); "
        "    Console.WriteLine(K.ToString()); "
        "    Console.WriteLine(\">\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("<\n7\n7\n>\n"));
}

// Shape (ii): an inline array's `.Length` as the ToString receiver. The
// `Console.WriteLine(a.Length)` control in the same program is the pre-empt:
// the bare read always worked (codegen folds N structurally), so only the
// chain-head position was broken.
BEZEL_TEST(codegen_console_tostring_untyped_receiver_3079,
           inline_array_length_tostring_prints_spec_15_1_2_8_5) {
    auto r = runAot(
        "module N3079B; "
        "class C "
        "  public static method Main(): int "
        "    var a: [16]int; "
        "    Console.WriteLine(\"<\"); "
        "    Console.WriteLine(a.Length); "
        "    Console.WriteLine(a.Length.ToString()); "
        "    Console.WriteLine(\">\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("<\n16\n16\n>\n"));
}

// The WHOLE surface the two type attachments enable, not just the reported
// `.ToString()` slice: every §8.5/stdlib §9.1 member reachable from an `int`
// receiver was dropped by the same gate, and a chain head is also an operand.
// Each observable contributes a distinct bit so a partial fix cannot pass.
//   1  bare const `.GetHashCode()`         2  bare const `.CompareTo(3)`
//   4  bare const `.Equals(7)`             8  `(K + 0).ToString()`
//  16  bare `const` of `string` `.Length`  32  bare const `.ToString(2)` radix
//  64  inline `.Length.GetHashCode()`     128  `(a.Length + 0).ToString()`
// 256  inline `.Length.CompareTo(4)`      512  bare const double `.ToString()`
BEZEL_TEST(codegen_console_tostring_untyped_receiver_3079,
           chain_head_member_surface_on_both_receivers_spec_22_12_15_1_2) {
    auto r = runAot(
        "module N3079C; "
        "class C "
        "  public const K: int := 7; "
        "  public const S: string := \"ab\"; "
        "  public const D: double := 1.5; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var a: [4]int; "
        "    if K.GetHashCode() = 7 then acc := acc + 1; end; "
        "    if K.CompareTo(3) = 1 then acc := acc + 2; end; "
        "    if K.Equals(7) then acc := acc + 4; end; "
        "    if (K + 0).ToString() = \"7\" then acc := acc + 8; end; "
        "    if S.Length = 2 then acc := acc + 16; end; "
        "    if K.ToString(2) = \"111\" then acc := acc + 32; end; "
        "    if a.Length.GetHashCode() = 4 then acc := acc + 64; end; "
        "    if (a.Length + 0).ToString() = \"4\" then acc := acc + 128; end; "
        "    if a.Length.CompareTo(4) = 0 then acc := acc + 256; end; "
        "    if D.ToString() = \"1.5\" then acc := acc + 512; end; "
        "    Console.WriteLine(acc); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1023\n"));
}

// THE CLASS-LEVEL PROPERTY, on the value channel: a `Console.Write(Line)` whose
// argument fails to lower must never again make the statement DISAPPEAR without
// a word. The §3/#605 permissive shape `o.Anything()` on an `Object` receiver is
// the one that still legitimately reaches the degrade (sema admits it BY
// DESIGN), so it is the probe: the compile still succeeds and the surrounding
// lines still print — the guard added by this fix is a Warning on that path, not
// a reject. The diagnostic TEXT is pinned by the sibling lit test
// `console_arg_lowering_silent_drop_3079.bzl`.
BEZEL_TEST(codegen_console_tostring_untyped_receiver_3079,
           permissive_unloweable_console_arg_still_compiles_spec_21_9) {
    auto r = runAot(
        "module N3079D; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := \"x\"; "
        "    Console.WriteLine(\"<\"); "
        "    Console.WriteLine(o.Anything()); "
        "    Console.WriteLine(\">\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("<\n>\n"));
}
