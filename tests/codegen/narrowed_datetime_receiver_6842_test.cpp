// Regression test for issue #6842.
//
// spec §7.4 (v0.340): member access on a nullable value is a compile error
// unless the compiler has narrowed it to non-null; §22.6: "a read of a binding
// §7.4 has narrowed non-null is of its non-nullable type". stdlib §2.5
// (#1114): DateTime.ToString() is "yyyy-MM-ddTHH:mm:ss"; TimeSpan, DateOnly and
// TimeOnly render by their own ToString. A narrowed `DateTime?` (and TimeSpan?,
// DateOnly?, TimeOnly?) receiver still loaded its {i8 hasValue; T} carrier, so
// `n.ToString()`, `n.ToString(fmt)`, `n.AddDays(..)` and `n.Year` passed the
// carrier to a runtime helper taking the bare scalar and failed IR verification.
// The un-narrowed form is rejected by sema (control row).
#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `expr` is evaluated 50 times over a guarded nullable parameter; the program
// prints the last rendering and the number of iterations that disagreed.
std::string prog(const std::string& ptype, const std::string& arg,
                 const std::string& expr, const std::string& want) {
    return "module M; class C "
           "  static method F(n: " + ptype + "): string "
           "    if n <> null then return " + expr + "; end; return \"none\"; "
           "  end F; "
           "  public static method Main(): int "
           "    var bad := 0; var s := \"\"; var i := 0; "
           "    while i < 50 do "
           "      s := F(" + arg + "); "
           "      if s <> \"" + want + "\" then bad := bad + 1; end; "
           "      i := i + 1; "
           "    end; "
           "    Console.WriteLine(s + \" \" + bad.ToString()); "
           "    return 0; "
           "  end Main; end C; ";
}

void expectOut(const std::string& ptype, const std::string& arg,
               const std::string& expr, const std::string& want) {
    auto r = runAotChecked(prog(ptype, arg, expr, want));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, want + " 0\n");
}

const char* kDt = "new DateTime(2024, 3, 15, 13, 5, 9)";

} // namespace

BEZEL_TEST(codegen_narrowed_datetime_receiver_6842, datetime_tostring) {
    expectOut("DateTime?", kDt, "n.ToString()", "2024-03-15T13:05:09");
}

BEZEL_TEST(codegen_narrowed_datetime_receiver_6842, datetime_tostring_format) {
    expectOut("DateTime?", kDt, "n.ToString(\"yyyy\")", "2024");
}

BEZEL_TEST(codegen_narrowed_datetime_receiver_6842, datetime_in_hole_member) {
    expectOut("DateTime?", kDt, "$\"[{n.ToString()}]\"", "[2024-03-15T13:05:09]");
}

BEZEL_TEST(codegen_narrowed_datetime_receiver_6842, datetime_method_and_property) {
    expectOut("DateTime?", kDt, "n.AddDays(1.0).ToString()", "2024-03-16T13:05:09");
    expectOut("DateTime?", kDt, "n.Year.ToString()", "2024");
}

BEZEL_TEST(codegen_narrowed_datetime_receiver_6842, timespan_tostring) {
    expectOut("TimeSpan?", "TimeSpan.FromSeconds(5.0)", "n.ToString()", "00:00:05");
}

BEZEL_TEST(codegen_narrowed_datetime_receiver_6842, dateonly_tostring) {
    expectOut("DateOnly?", "DateOnly.Parse(\"2024-03-15\")", "n.ToString()", "2024-03-15");
}

BEZEL_TEST(codegen_narrowed_datetime_receiver_6842, timeonly_tostring) {
    expectOut("TimeOnly?", "TimeOnly.Parse(\"13:05:09\")", "n.ToString()", "13:05:09");
}

// A declaration-narrowed local (the issue's repro shape: §7.4 "from a
// declaration whose initializer is such an expr").
BEZEL_TEST(codegen_narrowed_datetime_receiver_6842, declaration_narrowed_local) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var dt := new DateTime(2024, 3, 15, 13, 5, 9, 123); "
        "  var nn: DateTime? := dt; "
        "  Console.WriteLine(nn.ToString()); "
        "  Console.WriteLine($\"{nn} {nn.ToString()}\"); "
        "  return 0; end Main; end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "2024-03-15T13:05:09\n2024-03-15T13:05:09 2024-03-15T13:05:09\n");
}

// Control: the un-narrowed receiver stays a §7.4 compile error (the gate this
// fix relies on), never an IR verification failure.
BEZEL_TEST(codegen_narrowed_datetime_receiver_6842, unnarrowed_receiver_rejected) {
    auto r = runAot(
        "module M; class C "
        "  static method F(n: DateTime?): string return n.ToString(); end F; "
        "  public static method Main(): int return 0; end Main; end C; ");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        bezel::test::failedAtCompileTimeWithRealLocation(r) &&
        bezel::test::diagnosticMessageContains(
            r, "on a nullable value requires narrowing it to non-null first (§7.4)"));
}
