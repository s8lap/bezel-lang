// fix #8733 (spec §14.2 bounds table, stdlib §2.5/§2.6, v0.359): a
// `where T: value` bound at a TYPE-INSTANTIATION site admits a stdlib value
// builtin. (The call-site arm lives in ExprTypecheck_Calls.cpp; narrow #8733.)
//
// SPEC BASIS (QUOTED, v0.359). §14.2: "`where T: value` | Non-nullable value
// types -- records, enums, choices, primitives, inline arrays, the unit type."
// stdlib §2.5/§2.6: `public record TimeSpan: ...`, `public record Guid: ...`.
//
// FAILS without the fix (MEASURED on the base: "type argument `TimeSpan` does
// not satisfy the constraint `where T: value` (§14.2)").

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
std::string out(const std::string& program) {
    JitResult r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}
}  // namespace

// #8733: the issue's literal repro at the call site is decided by the
// call-site arm (reported separately); the type-instantiation site is here.
BEZEL_TEST(sema_bound_conformance_8733, value_bound_admits_a_stdlib_value_builtin_at_an_instantiation) {
    BEZEL_EXPECT_EQ(out(R"(
module Conf;
public class Box<T> where T: value
  public method N(): int => 7;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<TimeSpan>();
    Console.WriteLine(b.N());
    return 0;
  end Main;
end Program;
)"),
                      "7\n");
}

