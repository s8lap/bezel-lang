// Regression for #7845 — §35.4 rule 3 (v0.355): a choice is value-like when
// "every payload is a value, a `string`, a runtime-managed type, or
// (transitively) a value-like record or choice". `DateTime`, `TimeSpan`,
// `DateOnly`, `TimeOnly` and `Guid` are stdlib §2.5/§2.6 value records, so a
// choice whose payloads are only those is value-like and auto-hashable
// (§35.7): `=` is available. Pre-fix the payload was classified
// borrow-bearing (the null-declaration builtin took the bare-parameter
// conservative arm) and `=` was rejected. Controls: a class payload stays
// borrow-bearing (`=` still rejected).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

std::string progFor(const char* payloadTy, const char* a, const char* b) {
    return std::string("module M; public choice Ev At(p: ") + payloadTy +
           "), Never; end Ev; class C public static method Main(): int "
           "var x := Ev.At(" + a + "); var y := Ev.At(" + b +
           "); var z := x; Console.WriteLine(x = z); "
           "Console.WriteLine(x = y); return 0; end Main; end C;";
}

void expectValueLike(const char* ty, const char* a, const char* b) {
    auto r = runAot(progFor(ty, a, b));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\n"));
}

}  // namespace

BEZEL_TEST(sema_choice_value_builtin_payload_7845, datetime_payload_equality_35_4) {
    expectValueLike("DateTime", "new DateTime(2020, 1, 2, 3, 4, 5)",
                    "new DateTime(2021, 1, 2, 3, 4, 5)");
}
BEZEL_TEST(sema_choice_value_builtin_payload_7845, timespan_payload_equality_35_4) {
    expectValueLike("TimeSpan", "TimeSpan.FromSeconds(1.0)",
                    "TimeSpan.FromSeconds(2.0)");
}
BEZEL_TEST(sema_choice_value_builtin_payload_7845, dateonly_payload_equality_35_4) {
    expectValueLike("DateOnly", "new DateOnly(2020, 1, 2)",
                    "new DateOnly(2021, 1, 2)");
}
BEZEL_TEST(sema_choice_value_builtin_payload_7845, timeonly_payload_equality_35_4) {
    expectValueLike("TimeOnly", "new TimeOnly(1, 2, 3)",
                    "new TimeOnly(1, 2, 4)");
}
BEZEL_TEST(sema_choice_value_builtin_payload_7845, guid_payload_equality_35_4) {
    expectValueLike("Guid", "Guid.Empty", "Guid.NewGuid()");
}
// CONTROL: a string payload stays value-like.
BEZEL_TEST(sema_choice_value_builtin_payload_7845, string_payload_control) {
    expectValueLike("string", "\"a\"", "\"b\"");
}
// CONTROL: a class payload is borrow-bearing; `=` stays rejected (§35.7).
BEZEL_TEST(sema_choice_value_builtin_payload_7845, class_payload_stays_borrow_bearing) {
    auto r = runAot(
        "module M; public class K end K; public choice Ev At(p: K), Never; "
        "end Ev; class C public static method Main(): int "
        "var k := new K(); var x := Ev.At(k); var y := x; "
        "Console.WriteLine(x = y); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("borrow-bearing") != std::string::npos) found = true;
    BEZEL_EXPECT_TRUE(found);
}
