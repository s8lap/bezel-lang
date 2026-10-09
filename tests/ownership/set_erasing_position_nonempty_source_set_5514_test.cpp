// Regression tests for issue #5514 (§28.3 closure bullet *Set-erasing positions*,
// v0.330) — a function value with a non-empty source set may not be passed to
// a set-erasing position (Object parameter, or bare type parameter substituted
// with a function type).  The rule is binary on non-empty vs capture-free:
// a capture-free function value carries the empty set and is admitted.
//
// Polarity table (verified on staging):
//   capture-bearing lambda → Object:       REJECTED
//   capture-free lambda → Object:          ACCEPTED
//   capture-bearing lambda → bare T:        REJECTED
//   capture-free lambda → bare T:          ACCEPTED
//   plain function-typed param → bare T:  ACCEPTED
//   property access → Object:              ACCEPTED

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Helper: Item with a method, for capture tests.
const char* kItemClass =
    "class Item "
    "  public constructor() end; "
    "  public method Value(): int => 7; "
    "end Item; ";

// Helper: Keeper with Object field, for capture rejection tests.
const char* kKeeperObjectClass =
    "class Keeper "
    "  public _o: Object?; "
    "  public constructor(o: Object) _o := o; end constructor; "
    "end Keeper; ";

// Helper: Util with bare type param, for bare-T tests.
const char* kUtilTakeClass =
    "class Util "
    "  public static method Take<T>(x: T) "
    "    Console.WriteLine(\"took\"); "
    "  end Take; "
    "end Util; ";

}  // namespace

// REJECT: capture-bearing lambda → Object parameter
BEZEL_TEST(set_erasing_position_5514,
           capturing_lambda_to_object_param_rejected) {
    auto r = runAotChecked(
        std::string("module M; ") + kKeeperObjectClass + kItemClass +
        "class Test "
        "  public static method Main(): int "
        "    using var item := new Item(); "
        "    var f: () -> int := lambda() => item.Value(); "
        "    var k: Keeper := new Keeper(f); "
        "    return 0; "
        "  end Main; "
        "end Test;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ACCEPT: capture-free lambda → Object parameter
BEZEL_TEST(set_erasing_position_5514,
           capture_free_lambda_to_object_param_accepted) {
    auto r = runAot(
        std::string("module M; ") + kKeeperObjectClass +
        "class Test "
        "  public static method Main(): int "
        "    var f: () -> int := lambda() => 42; "
        "    var k: Keeper := new Keeper(f); "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end Test;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ok\n");
}

// REJECT: capture-bearing lambda → bare type parameter
BEZEL_TEST(set_erasing_position_5514,
           capturing_lambda_to_bare_type_param_rejected) {
    auto r = runAotChecked(
        std::string("module M; ") + kUtilTakeClass + kItemClass +
        "class Test "
        "  public static method Main(): int "
        "    using var item := new Item(); "
        "    var f: () -> int := lambda() => item.Value(); "
        "    Util.Take(f); "
        "    return 0; "
        "  end Main; "
        "end Test;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ACCEPT: capture-free lambda → bare type parameter
BEZEL_TEST(set_erasing_position_5514,
           capture_free_lambda_to_bare_type_param_accepted) {
    auto r = runAot(
        std::string("module M; ") + kUtilTakeClass +
        "class Test "
        "  public static method Main(): int "
        "    var f: () -> int := lambda() => 42; "
        "    Util.Take(f); "
        "    return 0; "
        "  end Main; "
        "end Test;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ACCEPT: plain function-typed parameter → bare type parameter
// A plain function-typed parameter is binding, not a capture.
// The value it receives (if capture-free) has empty source set.
BEZEL_TEST(set_erasing_position_5514,
           plain_function_typed_param_to_bare_type_param_accepted) {
    auto r = runAot(
        std::string("module M; ") + kUtilTakeClass +
        "class Test "
        "  public static method Run(f: () -> int): int "
        "    Util.Take(f); "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var h: () -> int := lambda() => 5; "
        "    return Run(h); "
        "  end Main; "
        "end Test;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ACCEPT: property access → Object parameter
// A property is not a function value and does not carry a capture set.
BEZEL_TEST(set_erasing_position_5514,
           property_to_object_param_accepted) {
    auto r = runAot(
        std::string("module M; ") + kKeeperObjectClass +
        "class CancellationToken "
        "  public static property None: CancellationToken? => default(CancellationToken?); "
        "  public constructor() end; "
        "end CancellationToken; "
        "class Test "
        "  public static method Main(): int "
        "    var k: Keeper := new Keeper(CancellationToken.None); "
        "    return 0; "
        "  end Main; "
        "end Test;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// REJECT: capture-bearing lambda passed to bare type param in generic method
// (bare T instantiated with function type, like the Util.Take<U>(x: U) case)
BEZEL_TEST(set_erasing_position_5514,
           capturing_lambda_to_generic_method_bare_param_rejected) {
    auto r = runAotChecked(
        std::string("module M; ") + kUtilTakeClass + kItemClass +
        "class Test "
        "  public static method Main(): int "
        "    using var item := new Item(); "
        "    var f: () -> int := lambda() => item.Value(); "
        "    Util.Take<() -> int>(f); "
        "    return 0; "
        "  end Main; "
        "end Test;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
