// Spec-derived tests for fix #5821 — §14.2 two-presentation paragraph
// (v0.338; stdlib §4.14): the value-typed `OwningBuffer<T>` presentation's
// single-argument constructor requires `T` to be defaultable, because it
// initializes every slot to `default(T)`, and `default(T)` is a compile error
// for a non-defaultable type (§17.11).
//
// `new OwningBuffer<Level>(4)` with `enum Level` declaring no zero-valued
// constant compiled with only an auto-drop warning before this fix. It is now
// rejected at the constructor, naming the enum and the §14.2 / §17.11 rule.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// REJECT — the issue's concrete row: the enum declares no zero-valued constant,
// so `default(Level)` is not defined and the one-argument constructor is
// rejected at the constructor with the specific diagnostic.
BEZEL_TEST(sema_owning_buffer_defaultable_5821,
           owning_buffer_one_arg_enum_without_zero_rejected_spec_14_2) {
    auto r = runAot(
        "module M; "
        "enum Level Low := 1, High := 2; end Level; "
        "class Main "
        "  public static method Main(): int "
        "    using var b := new OwningBuffer<Level>(4); "
        "    return b.Length; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "`new OwningBuffer<Level>(n)` with one argument is not admitted"));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "enum `Level` declares no constant of underlying value 0"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§14.2"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.11"));
}

// ACCEPT control — `int` is defaultable, so the one-argument constructor is
// admitted and `Length` reads back the requested capacity.
BEZEL_TEST(sema_owning_buffer_defaultable_5821,
           owning_buffer_one_arg_int_accepted_control_spec_14_2) {
    auto r = runAot(
        "module M; "
        "class Main "
        "  public static method Main(): int "
        "    using var b := new OwningBuffer<int>(4); "
        "    return b.Length; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 4);
}

// ACCEPT control — the same enum shape WITH a zero-valued constant is
// defaultable, so the rule (not the enum's shape alone) is what decides.
BEZEL_TEST(sema_owning_buffer_defaultable_5821,
           owning_buffer_one_arg_enum_with_zero_accepted_control_spec_14_2) {
    auto r = runAot(
        "module M; "
        "enum Level Zero := 0, High := 2; end Level; "
        "class Main "
        "  public static method Main(): int "
        "    using var b := new OwningBuffer<Level>(4); "
        "    return b.Length; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 4);
}
