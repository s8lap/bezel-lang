// Spec-derived regression test for issue #7291.
//
// stdlib §2.3 (v0.345) declares `ToString(value: int): string` (and the
// `long` / `float` / `double` / `decimal` forms beside it). §5.1 (v0.345):
// "For any type `T`, `T` implicitly converts to `T?`" — never the other
// way — so a `T?` reaches no `T` parameter.
//
// BEFORE THE FIX (the issue's own measurement, on the R35 #7286 mirror
// lane's base arm 8c510375 and its fix arm, and reproduced by the fix
// commits' authors on their parent arms): `Convert.ToString(v)` with
// `v: int?` that is NULL AT RUN TIME ABORTED the compiler (the value
// reached the lowering as the `{i8 hasValue, T}` carrier and the renderer
// read it as a value that is not one).
//
// THE FIX is in two parts. The §7 nullability gate's claim on the
// `Convert.ToString` VALUE position — the §7.3 nullability bit on that
// member's rows of the ONE built-in parameter table
// (`common::kBuiltinParams`, src/common/BuiltinRegistry.cpp) — landed in
// `68d5cbe3` ("fold the Math/Convert surface into the ONE declared-signature
// table"); that claim is what refuses the still-nullable argument at
// COMPILE TIME, with §7's rule: a still-nullable value reaches no
// non-nullable parameter. The §7.4-NARROWED payload at that position landed
// in `d2ad8fda` ("fix #7297 fix #7232: extract the §7.4-narrowed payload at
// a Math/Convert parameter"), so a NARROWED nullable still renders its
// payload.
//
// THE ORACLE. `runAot` — the strict AOT path. A rejection row asserts the
// FULL diagnostic at a REAL source location. A row that asserts only "the
// compiler did not abort" would be satisfied by a silent miscompile.
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

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// A nullable argument that is NULL AT RUN TIME reaches no `int` parameter:
// the call is a compile error, not a crash.
BEZEL_TEST(sema_convert_tostring_nullable_value_7291,
           a_null_at_run_time_nullable_value_is_refused) {
    auto r = runAot(prog(
        "    var v: int? := null;\n",
        "Console.WriteLine(Convert.ToString(v));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(
        r, "the argument to `Convert.ToString` is nullable but the target "
           "type is non-nullable (§7)"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// The SAME shape at the `toBase` form (`ToString(value: int, toBase: int)`).
BEZEL_TEST(sema_convert_tostring_nullable_value_7291,
           a_null_at_run_time_nullable_value_at_the_radix_form_is_refused) {
    auto r = runAot(prog(
        "    var v: int? := null;\n",
        "Console.WriteLine(Convert.ToString(v, 16));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(
        r, "the argument to `Convert.ToString` is nullable but the target "
           "type is non-nullable (§7)"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A NARROWED nullable is a different shape: §7.4 has discharged the null,
// so the payload reaches the parameter and is rendered.
BEZEL_TEST(sema_convert_tostring_nullable_value_7291,
           a_narrowed_nullable_value_renders_its_payload) {
    auto r = runAot(prog(
        "    var v: int? := 7;\n"
        "    if v <> null then\n",
        "Console.WriteLine(Convert.ToString(v)); end if;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "7\n");
}
