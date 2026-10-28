// Spec-derived regression test for issue #7234.
//
// stdlib §2.3 (v0.345) declares the `Convert.ToString` overload set
// `ToString(value: int)`, `ToString(value: long)`, `ToString(value: float)`,
// `ToString(value: double)` and `ToString(value: decimal)` — and the
// AMBIGUITY note beside them:
//
//   "An argument that converts to two or more of these parameter types and
//    matches none exactly is ambiguous and a compile error."
//
// §5.1 (v0.345) widens `char` to `int`, `uint`, `long`, `ulong`, `float`,
// `double` and `decimal` — so a `char` converts to SEVEN of the declared
// parameter types and matches none of them exactly. A `uint` converts to
// `long`, `ulong`, `float`, `double` and `decimal` — five of them, and none
// exactly.
//
// BEFORE THE FIX (the issue's own measurement, and reproduced by the fix
// commit's author on that commit's parent arm 39c77c9b):
// `Convert.ToString('A')` and `Convert.ToString(255U)` were ACCEPTED — the
// ambiguity note was not implemented.
//
// THE FIX (picked as `d7d31fa5`, "fix #7266: stdlib §2.3's overload-ambiguity
// rule now reaches byte/ushort/sbyte/char/short") adds §22.6's RANKING step
// and its last step (the AMBIGUITY error) over the declared forms the §2.3
// probe already collects, and the same step reaches the `Math` class
// (#7289).
//
// THE ORACLE. `runAot` — the strict AOT path. A rejection row asserts the
// FULL diagnostic at a REAL source location.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& stmt) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    " + stmt + "\n"
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

const char kAmbiguous[] =
    "ambiguous call to `Convert.ToString`: no overload is unambiguously "
    "better (§22.6)";

}  // namespace

// stdlib §2.3's ambiguity note: a `char` reaches SEVEN of the declared
// parameter types and matches none exactly.
BEZEL_TEST(sema_convert_tostring_ambiguity_char_uint_7234,
           a_char_argument_is_ambiguous) {
    auto r = runAot(prog("Console.WriteLine(Convert.ToString('A'));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, kAmbiguous));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// The same note, for a `uint`.
BEZEL_TEST(sema_convert_tostring_ambiguity_char_uint_7234,
           a_uint_argument_is_ambiguous) {
    auto r = runAot(prog("Console.WriteLine(Convert.ToString(255U));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, kAmbiguous));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// CONTROLS: an argument that matches ONE declared parameter type exactly is
// not ambiguous.
BEZEL_TEST(sema_convert_tostring_ambiguity_char_uint_7234,
           an_int_argument_is_not_ambiguous) {
    auto r = runAot(prog("Console.WriteLine(Convert.ToString(255));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "255\n");
}

BEZEL_TEST(sema_convert_tostring_ambiguity_char_uint_7234,
           a_long_argument_is_not_ambiguous) {
    auto r = runAot(prog("Console.WriteLine(Convert.ToString(255L));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "255\n");
}
