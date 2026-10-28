// Spec-derived regression test for issue #7331.
//
// stdlib §2.3 (v0.345) declares `ToString(value: int): string` and
// `ToString(value: int, toBase: int): string`. A §7.4-NARROWED nullable
// operand reaches the parameter as its PAYLOAD, and the member renders the
// payload — never the carrier (the box the nullable is held in).
//
// BEFORE THE FIX (the issue's own measurement, and reproduced by the fix
// commit's author on that commit's parent arm 2f4d5913): `Convert.ToString(v)`
// with `v: int?` RENDERED THE CARRIER's bytes, and `Convert.ToString(v, 16)`
// on a nullable operand ABORTED the compiler.
//
// THE FIX (picked as `d2ad8fda`, "fix #7297 fix #7232: extract the §7.4-
// narrowed payload at a Math/Convert parameter") extracts the PAYLOAD of the
// `{i8 hasValue; T}` carrier at the `Convert` parameter, and the §7
// nullability gate's claim on the value position (the §7.3 nullability bit
// on the `Convert.ToString` rows of the ONE built-in parameter table) landed
// with `68d5cbe3`.
//
// THE ORACLE. `runAot` — the strict AOT path. A value row asserts the
// RENDERED TEXT: a row that asserted only "the compiler did not abort" would
// be satisfied by the carrier's bytes.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& decls, const std::string& stmt) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" +
           decls + "    " + stmt + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// stdlib §2.3: `ToString(value: int): string` — the payload, in decimal.
BEZEL_TEST(sema_convert_tostring_nullable_operand_7331,
           a_nullable_operand_renders_its_payload) {
    auto r = runAot(prog(
        "    var v: int? := 255;\n"
        "    if v <> null then\n",
        "Console.WriteLine(Convert.ToString(v)); end if;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "255\n");
}

// stdlib §2.3: `ToString(value: int, toBase: int): string` — the payload, in
// the radix form. The carrier's bytes would be a pointer, never "ff".
BEZEL_TEST(sema_convert_tostring_nullable_operand_7331,
           a_nullable_operand_at_the_radix_form_renders_its_payload) {
    auto r = runAot(prog(
        "    var v: int? := 255;\n"
        "    if v <> null then\n",
        "Console.WriteLine(Convert.ToString(v, 16)); end if;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "ff\n");
}

// A `double?` payload renders the double's own text.
BEZEL_TEST(sema_convert_tostring_nullable_operand_7331,
           a_nullable_double_operand_renders_its_payload) {
    auto r = runAot(prog(
        "    var v: double? := 2.5;\n"
        "    if v <> null then\n",
        "Console.WriteLine(Convert.ToString(v)); end if;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2.5\n");
}
