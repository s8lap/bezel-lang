// fix #8728 (spec §14.2 *Interface conformance* rule 2 and *Constraints on
// overrides and interface implementations*, §27.8, v0.359): the §14.2
// constraint's instantiation is compared after type-alias resolution.
//
// SPEC BASIS (QUOTED, v0.359). §27.8: "a type alias is a pure synonym -- it
// does not create a new type"; §14.2 compares a constraint's instantiation
// "after type-alias resolution (§27.8)".
//
// FAILS without the fix (MEASURED on the base r43-f45-toolchain: "type
// argument `Rec` does not satisfy the constraint `where T: IComparable`").
// Must-still-admit twins (the alias spelled directly) are corpus candidates.

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

// #8728: the issue's literal repro. A user alias of a built-in names the
// built-in; the candidate declares `IComparable<DateTime>`.
BEZEL_TEST(sema_bound_conformance_8728, alias_of_builtin_in_bound_names_the_builtin) {
    BEZEL_EXPECT_EQ(out(R"(
module Conf;
type MyDT := DateTime;
public class Program
  public static method Main(): int
    return F(new Rec());
  end Main;
  public static method F<T>(x: T): int where T: IComparable<MyDT>
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
public record Rec: IComparable<DateTime>
  public method CompareTo(other: DateTime): int
    return 0;
  end CompareTo;
end Rec;
)"),
                      "accepted\n");
}

