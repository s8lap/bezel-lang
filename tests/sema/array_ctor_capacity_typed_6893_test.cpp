// fix #6893 -- sema: `new Array<T>(x)` took any one argument as the
// `capacity: int` constructor (stdlib §4.4, v0.340). Array<T> declares only
// (), (capacity: int) and (size: int, fill: T); there is no copying constructor.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
bool rejectedWith(const bezel::test::JitResult& r, const std::string& needle) {
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

BEZEL_TEST(sema_array_ctor_capacity_typed_6893, array_ctor_array_argument_4_4) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [3, 1, 2]; var c := new Array<int>(a); "
        "  return c.Count; end Main; end C;");
    BEZEL_EXPECT_TRUE(rejectedWith(r, "capacity argument of the `Array<T>` constructor"));
}
BEZEL_TEST(sema_array_ctor_capacity_typed_6893, array_ctor_int_forms_still_accepted_4_4) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var c := new Array<int>(5); var d := new Array<int>(3, 7); "
        "  return c.Count + d.Count; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3 + 0 + 0);
}
