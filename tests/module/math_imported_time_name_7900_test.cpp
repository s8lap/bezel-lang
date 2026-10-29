// Review fix-forward for issue #7900 (campaign builtin-call-admission, lane
// r37-c2-47-mathadmit): the Math.Min/Max/Clamp time-value admission matches the
// builtin DateTime / TimeSpan / DateOnly / TimeOnly by head name. An IMPORTED
// user type of the same name also has `declaration->decl == nullptr` and differs
// only by `moduleName`; it must NOT be admitted.
//
// stdlib §2.2: `Max<T>(a: T, b: T): T where T: IComparable<T>`.
// spec §14.2: a type satisfies an interface constraint only if it declares the
//   interface in its extension chain. A user `public record DateTime(v, w)` in
//   an imported module declares no IComparable, so `Math.Max(a, b)` over it is
//   refused with the constraint diagnostic (it used to print corrupt output;
//   an imported 4-field `TimeOnly` dumped core).
// spec §2.8.3 rule 3: a type is identified by its declaring module, so the
//   imported `Lib.DateTime` is unrelated to the stdlib `DateTime`.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotModules;
using bezel::test::diagnosticMessageContains;

namespace {

JitResult run(const std::string& lib, const std::string& body) {
    return runAotModules(
        {ModuleSource{"Lib", lib},
         ModuleSource{"App",
                      "module App; import Lib; class C "
                      "  public static method Main(): int " +
                          body +
                          "    return 0; end Main; end C;"}},
        "App");
}

bool refusedWithConstraint(const JitResult& r) {
    return !r.ran &&
           diagnosticMessageContains(r, "does not satisfy the constraint");
}

}  // namespace

BEZEL_TEST(math_imported_time_name_7900, imported_non_comparable_datetime_refused_2_2) {
    const JitResult r = run(
        "module Lib; "
        "public record DateTime(v: int, w: int); "
        "public class Mk "
        "  public static method DT(v: int, w: int): DateTime "
        "    return new DateTime(v, w); "
        "  end DT; "
        "end Mk;",
        "    var a := Mk.DT(5, 100); "
        "    var b := Mk.DT(3, 200); "
        "    var m := Math.Max(a, b); "
        "    Console.WriteLine(m.v);");
    BEZEL_EXPECT_TRUE(refusedWithConstraint(r));
}

BEZEL_TEST(math_imported_time_name_7900, imported_non_comparable_timeonly_refused_2_2) {
    const JitResult r = run(
        "module Lib; "
        "public record TimeOnly(name: string, a: int, b: long, c: long); "
        "public class Mk "
        "  public static method TO(n: string, a: int): TimeOnly "
        "    return new TimeOnly(n, a, 1L, 2L); "
        "  end TO; "
        "end Mk;",
        "    var a := Mk.TO(\"x\", 1); "
        "    var b := Mk.TO(\"y\", 2); "
        "    var m := Math.Max(a, b); "
        "    Console.WriteLine(m.name);");
    BEZEL_EXPECT_TRUE(refusedWithConstraint(r));
}

// CONTROL (mirror): the same name from an IComparable-declaring imported record
// is admitted by the user-type path, unchanged.
BEZEL_TEST(math_imported_time_name_7900, imported_comparable_timespan_still_admitted_2_2) {
    const JitResult r = run(
        "module Lib; "
        "public record TimeSpan: IComparable<TimeSpan> "
        "  public v: int; "
        "  public method CompareTo(other: TimeSpan): int return this.v - other.v; end CompareTo; "
        "end TimeSpan; "
        "public class Mk "
        "  public static method TS(v: int): TimeSpan return new TimeSpan(v); end TS; "
        "end Mk;",
        "    var a := Mk.TS(1); "
        "    var b := Mk.TS(2); "
        "    var m := Math.Max(a, b); "
        "    Console.WriteLine(m.v);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
