// Regression for #7712 -- `sb.Append(bx.V)` of an OWNING choice compiled,
// boxing to the `Append(value: Object)` overload.
//
// per spec §35.8 (v0.350): "Owning choices cannot be boxed -- §8.6's move-only
// rule", rejected at the conversion site. stdlib §3.1 declares
// `Append(value: Object)`; the StringBuilder builtins carry no MethodSymbol so
// the conversion-site check never ran (the Console sibling is #7694).
// Value-like choices keep appending via their §35.7 case-shaped ToString.

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
    "class P public static method Main() "
    "var sb := new StringBuilder(); ";

bool rejectedAsBox(const std::string& body) {
    auto r = runAot(std::string(kPre) + body + " end Main; end P;");
    return !r.ran && bezel::test::diagnosticMessageContains(r, "cannot be boxed");
}
}  // namespace

BEZEL_TEST(stringbuilder_append_owning_choice_7712, field_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsBox("var bx := new Bx(); sb.Append(bx.V);"));
}
BEZEL_TEST(stringbuilder_append_owning_choice_7712, local_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsBox("var x := OC.Empty; sb.Append(x);"));
}
BEZEL_TEST(stringbuilder_append_owning_choice_7712, call_result_payload_case_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsBox("sb.Append(OC.Text(new Res()));"));
}
BEZEL_TEST(stringbuilder_append_owning_choice_7712, chained_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAsBox("var bx := new Bx(); sb.Append(\"a\").Append(bx.V);"));
}

// Controls: value-like choice and the declared overloads still append.
BEZEL_TEST(stringbuilder_append_owning_choice_7712, value_like_and_declared_overloads_still_append) {
    auto r = runAot(std::string(kPre) +
        "var bx := new Bx(); sb.Append(bx.C); sb.Append(\"|\"); sb.Append(VC.B); "
        "sb.Append(\"|\"); sb.Append(5); sb.Append('c'); "
        "Console.WriteLine(sb.ToString()); end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("VC.A(x: 3)|VC.B|5c\n"));
}
