// Codegen — §14.2 / §8.6 / stdlib §2.5 / #7875 (spec v0.357): a BOXED built-in
// temporal value type viewed through `IComparable<T>` / `IEquatable<T>` must
// DISPATCH to the type's own CompareTo / Equals.
//
// THE DEFECT (MEASURED on the parent): `var ic: IComparable<DateTime> := dt;
// ic.CompareTo(x)` compiled and then SEGV'd (rc=139): the value-builtin box
// vtable carried the shared EMPTY itmap, so the interface call's itable lookup
// found nothing and the call went through a null slot. Same for TimeSpan,
// DateOnly and TimeOnly (stdlib §2.5 declares each `: IComparable<Self>,
// IEquatable<Self>`). Guid (stdlib §2.6) is tracked by #8288.
//
// Oracle: exact stdout (the result of CompareTo / Equals on values that
// differ, are equal, and are reversed), under the checked (ASan) leg since
// the receiver is a box.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string cmpProgram(const char* ty, const char* lo, const char* hi) {
    std::string t = ty;
    return std::string("module M; public class C "
                       "  public static method Main(): int ") +
           "    var lo := " + lo + "; var hi := " + hi + "; var lo2 := " + lo +
           "; "
           "    var a: IComparable<" + t + "> := lo; "
           "    var b: IComparable<" + t + "> := hi; "
           "    Console.WriteLine(a.CompareTo(hi)); "
           "    Console.WriteLine(b.CompareTo(lo)); "
           "    Console.WriteLine(a.CompareTo(lo2)); "
           "    var e: IEquatable<" + t + "> := lo; "
           "    Console.WriteLine(e.Equals(lo2)); "
           "    Console.WriteLine(e.Equals(hi)); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

void expectRow(const std::string& src) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n1\n0\nTrue\nFalse\n");
}

}  // namespace

// stdlib §2.5 DateTime: CompareTo = tick order.
BEZEL_TEST(codegen_boxed_value_builtin_icomparable_7875,
           datetime_through_icomparable_and_iequatable_stdlib_2_5) {
    expectRow(cmpProgram("DateTime", "new DateTime(2020,1,2,3,4,5)",
                         "new DateTime(2020,1,3,3,4,5)"));
}

// stdlib §2.5 TimeSpan: tick order, signed (negative span orders first).
BEZEL_TEST(codegen_boxed_value_builtin_icomparable_7875,
           timespan_through_icomparable_and_iequatable_stdlib_2_5) {
    expectRow(cmpProgram("TimeSpan", "new TimeSpan(-5)",
                         "new TimeSpan(6)"));
}

// stdlib §2.5 DateOnly: chronological.
BEZEL_TEST(codegen_boxed_value_builtin_icomparable_7875,
           dateonly_through_icomparable_and_iequatable_stdlib_2_5) {
    expectRow(cmpProgram("DateOnly", "new DateOnly(2020,1,2)",
                         "new DateOnly(2020,1,3)"));
}

// stdlib §2.5 TimeOnly.
BEZEL_TEST(codegen_boxed_value_builtin_icomparable_7875,
           timeonly_through_icomparable_and_iequatable_stdlib_2_5) {
    expectRow(cmpProgram("TimeOnly", "new TimeOnly(1,2)",
                         "new TimeOnly(1,3)"));
}
