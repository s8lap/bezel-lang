// Regression for #7694 -- `Console.WriteLine(bx.V)` of an OWNING choice
// compiled and printed an empty line.
//
// per spec §35.8 (v0.350): "Owning choices cannot be boxed -- §8.6's move-only
// rule"; the conversion of an owning value to `Object` is "rejected at the
// conversion site". stdlib §2.1 (v0.350): `WriteLine(value: Object)` and
// `Write(value: Object)` take `Object`, so an owning choice argument is a
// boxing and a compile error (a user-declared `Object` parameter already
// rejected it; the Console builtins carry no MethodSymbol and bypassed the
// check). The spelling for printing it is the author's own `ToString` member
// (§35.7 / §10.5) or a consuming match. Value-like choices stay admitted and
// print the §35.7 case-shaped form (spec §18.5 interpolation / §8.5).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
const char* kPre =
    "module M; "
    "class Res public constructor() end constructor; end Res; "
    "choice OC Text(owned r: Res), Empty; end OC; "
    "choice VC A(x: int), B; end VC; "
    "class Bx public owned V: OC := OC.Empty; public C: VC := VC.A(3); end Bx; "
    "class P public static method MkV(): VC => VC.A(9); "
    "public static method Main() ";

bool rejectedAsBox(const std::string& body) {
    auto r = runAot(std::string(kPre) + body + " end Main; end P;");
    return !r.ran && bezel::test::diagnosticMessageContains(r, "cannot be boxed");
}
}  // namespace

BEZEL_TEST(console_writeline_owning_choice_7694, field_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsBox("var bx := new Bx(); Console.WriteLine(bx.V);"));
}
BEZEL_TEST(console_writeline_owning_choice_7694, local_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsBox("var x := OC.Empty; Console.WriteLine(x);"));
}
BEZEL_TEST(console_writeline_owning_choice_7694, call_result_payload_case_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsBox("Console.WriteLine(OC.Text(new Res()));"));
}
BEZEL_TEST(console_writeline_owning_choice_7694, write_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsBox("var bx := new Bx(); Console.Write(bx.V);"));
}

// Value-like choice: every position keeps working and prints the §35.7 form.
BEZEL_TEST(console_writeline_owning_choice_7694, value_like_choice_still_prints) {
    auto r = runAot(std::string(kPre) +
        "var bx := new Bx(); var l := VC.A(1); var e := VC.B; "
        "Console.WriteLine(l); Console.WriteLine(e); Console.WriteLine(bx.C); "
        "Console.WriteLine(P.MkV()); Console.WriteLine($\"{l} {e} {bx.C} {P.MkV()}\"); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        std::string("VC.A(x: 1)\nVC.B\nVC.A(x: 3)\nVC.A(x: 9)\n"
                    "VC.A(x: 1) VC.B VC.A(x: 3) VC.A(x: 9)\n"));
}

// Interpolation of an owning choice stays the §35.7 use-site error.
BEZEL_TEST(console_writeline_owning_choice_7694, owning_interpolation_still_rejected) {
    auto r = runAot(std::string(kPre) +
        "var bx := new Bx(); var s := $\"{bx.V}\"; end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "not auto-hashable"));
}
