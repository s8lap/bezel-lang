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


// fix #8733 (CALL-SITE `value` arm, §14.2 bounds table, stdlib §2.5, v0.359):
// the issue's literal repro, a stdlib value record against `where T: value`.
BEZEL_TEST(sema_bound_conformance_8733, value_bound_admits_a_stdlib_value_builtin_at_a_call_site) {
    BEZEL_EXPECT_EQ(out(R"(
module Conf;
public class Program
  public static method Main(): int
    return F(TimeSpan.FromTicks(5L));
  end Main;
  public static method F<T>(x: T): int where T: value
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
)"),
                      "accepted\n");
}

// fix #9033 (§27.8 alias transparency, §14.2, stdlib §2.5, v0.359): an alias
// of a stdlib value record is admitted against `where T: value` exactly as
// the record is, at the instantiation site and the call site.
BEZEL_TEST(sema_bound_conformance_9033, value_bound_admits_an_alias_of_a_stdlib_value_builtin_at_an_instantiation) {
    BEZEL_EXPECT_EQ(out(R"(
module M;
type TS := TimeSpan;
public class Box<T> where T: value
  public method N(): int => 7;
end Box;
public class P
  public static method Main(): int
    var b := new Box<TS>();
    Console.WriteLine(b.N());
    return 0;
  end Main;
end P;
)"),
                      "7\n");
}

BEZEL_TEST(sema_bound_conformance_9033, value_bound_admits_an_alias_of_a_stdlib_value_builtin_at_a_call_site) {
    BEZEL_EXPECT_EQ(out(R"(
module M;
type TS := TimeSpan;
public class P
  public static method F<T>(x: T): int where T: value
    return 1;
  end F;
  public static method Main(): int
    var t: TS := TimeSpan.FromTicks(5L);
    Console.WriteLine(F(t));
    return 0;
  end Main;
end P;
)"),
                      "1\n");
}

BEZEL_TEST(sema_bound_conformance_9033, value_bound_admits_an_alias_of_a_stdlib_value_builtin_with_explicit_type_argument) {
    BEZEL_EXPECT_EQ(out(R"(
module M;
type TS := TimeSpan;
public class P
  public static method F<T>(x: T): int where T: value => 1;
  public static method Main(): int
    Console.WriteLine(F<TS>(TimeSpan.FromTicks(5L)));
    return 0;
  end Main;
end P;
)"),
                      "1\n");
}

// Controls (must not flip): an alias of a user class is still refused.
BEZEL_TEST(sema_bound_conformance_9033, value_bound_still_rejects_an_alias_of_a_user_class) {
    JitResult r = runAot(R"(
module M;
public class C end C; type CA := C;
public class P
  public static method F<T>(x: T): int where T: value
    return 1;
  end F;
  public static method Main(): int
    var c: CA := new C(); Console.WriteLine(F(c));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
