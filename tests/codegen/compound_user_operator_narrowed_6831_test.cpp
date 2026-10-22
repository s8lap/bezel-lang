// fix #6831 (§6.6, §7.11, §7.4, v0.340): a compound assignment through a
// user-defined operator with a §7.4-narrowed nullable record operand.
//
//   §6.6 (v0.340): "the statement `x op= e` is equivalent to `x := x op e`".
//   §7.11 (v0.340): "an operand that §7.4 has narrowed non-null is a
//   non-nullable operand and the ordinary operator applies to it unchanged".
//
// Before the fix the compound-assignment operator search read the operand's
// DECLARED `Pt?` (the primitive-only `operandStaticTypeAt` left a Named type
// alone), so `a += p` under `if p <> null` was rejected with "`Pt` declares no
// applicable `operator +` overload" -- although `a := a + p` is admitted
// (#6758) and the operator exists. Codegen also needs the `{i8, Pt}` carrier
// unwrapped, and a narrowed `T?` TARGET needs the result stored back as the
// carrier.
//
// Accepted rows read the computed fields (a carrier in place of the payload
// cannot hide); negative rows still reject an un-narrowed operand.

#include <string>

#include "support/aot_support.hpp"   // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kPt =
    "public record Pt(X: int, Y: int)\n"
    "  public static pure method operator +(a: Pt, b: Pt): Pt => new Pt(a.X + b.X, a.Y + b.Y);\n"
    "  public static pure method operator *(a: Pt, k: int): Pt => new Pt(a.X * k, a.Y * k);\n"
    "end Pt;\n";

std::string program(const std::string& mainBody,
                    const std::string& extraTypes = "") {
    return std::string("module M;\n") + kPt + extraTypes +
           "public class P\n"
           "  public static method Pick(b: bool): Pt?\n"
           "    if b then return new Pt(3, 2); end if;\n"
           "    return null;\n"
           "  end Pick;\n"
           "  public static method Main(): int\n" + mainBody +
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

std::string runOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        std::string s = "DID NOT RUN:";
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.severity == bezel::Severity::Error) s += " | " + d.message;
        return s;
    }
    return r.stdoutText + "exit=" + std::to_string(r.exitCode);
}

bool rejectedByCompoundRule(const std::string& src) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("compound assignment `+=` has no valid result") !=
            std::string::npos)
            return true;
    return false;
}

}  // namespace

// ---- the issue's own repro (k1.bzl), verbatim -------------------------------

// Expected 4. Before: "compound assignment `+=` has no valid result on record
// type `Pt` ... `Pt` declares no applicable `operator +` overload".
BEZEL_TEST(codegen_compound_user_operator_narrowed_6831, issue_repro_verbatim) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "public record Pt(X: int, Y: int)\n"
        "  public static pure method operator +(a: Pt, b: Pt): Pt => new Pt(a.X + b.X, a.Y + b.Y);\n"
        "end Pt;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var a: Pt := new Pt(1, 1);\n"
        "    var p: Pt? := new Pt(3, 2);\n"
        "    if p <> null then\n"
        "      a += p;\n"
        "      Console.WriteLine(a.X);\n"
        "    end if;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n"),
        std::string("4\nexit=0"));
}

// ---- accepted ---------------------------------------------------------------

BEZEL_TEST(codegen_compound_user_operator_narrowed_6831, rhs_narrowed_both_fields) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var a: Pt := new Pt(1, 1); var p: Pt? := P.Pick(true);\n"
        "    if p <> null then a += p; Console.WriteLine(a.X); Console.WriteLine(a.Y); end if;\n")),
        std::string("4\n3\nexit=0"));
}

// A narrowed `T?` TARGET: the operator reads its payload and the result is
// stored back as the carrier (read after the block through `??`).
BEZEL_TEST(codegen_compound_user_operator_narrowed_6831, target_narrowed_store_back) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var a: Pt := new Pt(1, 1); var p: Pt? := P.Pick(true);\n"
        "    if p <> null then p += a; Console.WriteLine(p.X); end if;\n"
        "    Console.WriteLine((p ?? new Pt(0, 0)).Y);\n")),
        std::string("4\n3\nexit=0"));
}

BEZEL_TEST(codegen_compound_user_operator_narrowed_6831, target_narrowed_int_rhs) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var p: Pt? := P.Pick(true);\n"
        "    if p = null then return 1; end if;\n"
        "    p *= 5; Console.WriteLine(p.Y);\n")),
        std::string("10\nexit=0"));
}

BEZEL_TEST(codegen_compound_user_operator_narrowed_6831, both_narrowed_and) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var p: Pt? := P.Pick(true); var q: Pt? := P.Pick(true);\n"
        "    if p <> null and q <> null then p += q; Console.WriteLine(p.X); end if;\n")),
        std::string("6\nexit=0"));
}

BEZEL_TEST(codegen_compound_user_operator_narrowed_6831, early_exit_in_loop) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var a: Pt := new Pt(1, 1); var p: Pt? := P.Pick(true);\n"
        "    if p = null then return 1; end if;\n"
        "    for var i in 0 ..< 100 do a += p; end;\n"
        "    Console.WriteLine(a.X);\n")),
        std::string("301\nexit=0"));
}

BEZEL_TEST(codegen_compound_user_operator_narrowed_6831, array_and_dictionary_element_targets) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    var arr := new Array<Pt>(2, new Pt(1, 1)); var p: Pt? := P.Pick(true);\n"
        "    var d := new Dictionary<int, Pt>(); d[1] := new Pt(10, 10);\n"
        "    if p <> null then arr[0] += p; d[1] += p; Console.WriteLine(arr[0].X); Console.WriteLine(d[1].Y); end if;\n")),
        std::string("4\n12\nexit=0"));
}

BEZEL_TEST(codegen_compound_user_operator_narrowed_6831, field_and_property_targets) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    using var h := new H(); var p: Pt? := P.Pick(true);\n"
        "    if p <> null then h.F += p; h.Prop += p; Console.WriteLine(h.F.X); Console.WriteLine(h.Prop.X); end if;\n",
        "public class H\n"
        "  public F: Pt;\n"
        "  public property Prop: Pt;\n"
        "  public constructor() F := new Pt(1, 1); Prop := new Pt(10, 10); end constructor;\n"
        "end H;\n")),
        std::string("4\n13\nexit=0"));
}

// The declaration-initializer narrowing (§7.4) of the shape
// sema_compound_operator_overload_tie_5205.t6 used to reject; that row now
// takes its target from a nullable-typed call.
BEZEL_TEST(codegen_compound_user_operator_narrowed_6831, declaration_initializer_target_short_rhs) {
    BEZEL_EXPECT_EQ(runOut(
        "module M;\n"
        "record V(X: int)\n"
        "  public static method operator +(v: V, b: short): V => new V(v.X + int(b));\n"
        "  public static method operator +(v: V, n: long): V => new V(v.X + 1000);\n"
        "end V;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var v: V? := new V(1); var s: short := 7; v += s;\n"
        "    Console.WriteLine(v.X);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n"),
        std::string("8\nexit=0"));
}

// ---- negative: un-narrowed stays rejected --------------------------------------

BEZEL_TEST(codegen_compound_user_operator_narrowed_6831, neg_rhs_unnarrowed) {
    BEZEL_EXPECT_TRUE(rejectedByCompoundRule(program(
        "    var a: Pt := new Pt(1, 1); var p: Pt? := P.Pick(true);\n"
        "    a += p; Console.WriteLine(a.X);\n")));
}

BEZEL_TEST(codegen_compound_user_operator_narrowed_6831, neg_target_unnarrowed) {
    BEZEL_EXPECT_TRUE(rejectedByCompoundRule(program(
        "    var a: Pt := new Pt(1, 1); var p: Pt? := P.Pick(true);\n"
        "    p += a; Console.WriteLine((p ?? a).X);\n")));
}

BEZEL_TEST(codegen_compound_user_operator_narrowed_6831, neg_rhs_reassigned_after_test) {
    BEZEL_EXPECT_TRUE(rejectedByCompoundRule(program(
        "    var a: Pt := new Pt(1, 1); var p: Pt? := P.Pick(true);\n"
        "    if p <> null then p := P.Pick(false); a += p; Console.WriteLine(a.X); end if;\n")));
}
