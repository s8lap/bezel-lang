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

// REJECT — a choice element: `default(Shape)` is not defined (§35.10 / §17.11),
// so the one-argument constructor is rejected with the choice-specific reason.
BEZEL_TEST(sema_owning_buffer_defaultable_5821,
           owning_buffer_one_arg_choice_rejected_spec_14_2) {
    auto r = runAot(
        "module M; "
        "choice Shape "
        "  Circle(r: int), "
        "  Square(s: int); "
        "end Shape; "
        "class Main "
        "  public static method Main(): int "
        "    using var b := new OwningBuffer<Shape>(4); "
        "    return b.Length; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "`new OwningBuffer<Shape>(n)` with one argument is not admitted"));
    BEZEL_EXPECT_TRUE(diagContains(r, "a choice type has no default (§35.10)"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§14.2"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.11"));
}

// REJECT — a record with a non-nullable `string` field: the record is not
// defaultable (§17.11, a record carrying a non-nullable reference field), so
// the one-argument constructor is rejected naming the record.
BEZEL_TEST(sema_owning_buffer_defaultable_5821,
           owning_buffer_one_arg_string_record_rejected_spec_17_11) {
    auto r = runAot(
        "module M; "
        "record Name(s: string); "
        "class Main "
        "  public static method Main(): int "
        "    using var b := new OwningBuffer<Name>(4); "
        "    return b.Length; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "`new OwningBuffer<Name>(n)` with one argument is not admitted"));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "record `Name` has a non-defaultable component"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§14.2"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.11"));
}

// REJECT — a constructor-only record (§9.9): a body-form record whose
// constructor carries a `requires` clause has no zero value, so the
// one-argument constructor is rejected naming the constructor-only property.
BEZEL_TEST(sema_owning_buffer_defaultable_5821,
           owning_buffer_one_arg_constructor_only_rejected_spec_9_9) {
    auto r = runAot(
        "module M; "
        "record Port "
        "  public readonly property Value: int; "
        "  public constructor(value: int) "
        "    requires(value >= 0 and value <= 65535) "
        "    Value := value; "
        "  end constructor; "
        "end Port; "
        "class Main "
        "  public static method Main(): int "
        "    using var b := new OwningBuffer<Port>(4); "
        "    return b.Length; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "`new OwningBuffer<Port>(n)` with one argument is not admitted"));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "record `Port` has a non-defaultable component or is constructor-only"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.11"));
}

// REJECT — fix #5821 (§14.2 two-presentation paragraph, v0.338): a generic body
// that constructs `OwningBuffer<T>(n)` at its own `T` is read at each
// instantiation. `H<Level>` supplies a non-defaultable enum, so the
// instantiation is rejected, naming the constructor and the parameter's
// `default(T)` use (line 5 of the body).
BEZEL_TEST(sema_owning_buffer_defaultable_5821,
           owning_buffer_generic_body_instantiated_at_non_defaultable_rejected_spec_14_2) {
    auto r = runAot(
        "module M; "
        "enum Level Low := 1, High := 2; end Level; "
        "class H<T> "
        "  public static method Mk(n: int): int "
        "    using var b := new OwningBuffer<T>(n); "
        "    return b.Length; "
        "  end Mk; "
        "end H; "
        "class Main "
        "  public static method Main(): int "
        "    return H<Level>.Mk(4); "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "does not satisfy the one-argument `OwningBuffer<T>(n)` constructor"));
    BEZEL_EXPECT_TRUE(diagContains(r, "`Level`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§14.2"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.11"));
}

// ACCEPT control — the generic body instantiated at `int` (defaultable) runs.
BEZEL_TEST(sema_owning_buffer_defaultable_5821,
           owning_buffer_generic_body_instantiated_at_int_accepted_control_spec_14_2) {
    auto r = runAot(
        "module M; "
        "class H<T> "
        "  public static method Mk(n: int): int "
        "    using var b := new OwningBuffer<T>(n); "
        "    return b.Length; "
        "  end Mk; "
        "end H; "
        "class Main "
        "  public static method Main(): int "
        "    return H<int>.Mk(4); "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 4);
}

// ACCEPT control — the generic body instantiated at an enum WITH a zero
// constant is defaultable, so the instantiation is admitted and runs.
BEZEL_TEST(sema_owning_buffer_defaultable_5821,
           owning_buffer_generic_body_instantiated_at_defaultable_enum_accepted_control_spec_14_2) {
    auto r = runAot(
        "module M; "
        "enum Level Low := 0, High := 2; end Level; "
        "class H<T> "
        "  public static method Mk(n: int): int "
        "    using var b := new OwningBuffer<T>(n); "
        "    return b.Length; "
        "  end Mk; "
        "end H; "
        "class Main "
        "  public static method Main(): int "
        "    return H<Level>.Mk(4); "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 4);
}

// ACCEPT control — a reference-typed instantiation keeps the class presentation
// (no default needed), so the generic body is admitted at a class.
BEZEL_TEST(sema_owning_buffer_defaultable_5821,
           owning_buffer_generic_body_instantiated_at_class_accepted_control_spec_14_2) {
    auto r = runAot(
        "module M; "
        "class Ref end Ref; "
        "class H<T> "
        "  public static method Mk(n: int): int "
        "    using var b := new OwningBuffer<T>(n); "
        "    return b.Length; "
        "  end Mk; "
        "end H; "
        "class Main "
        "  public static method Main(): int "
        "    return H<Ref?>.Mk(4); "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 4);
}
