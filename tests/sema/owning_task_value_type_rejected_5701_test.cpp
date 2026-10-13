// fix #5701 (§14.4 "Class-only restriction on owning generics", v0.338): the
// standard-library owning type `OwningTask<T>` restricts its element type T to
// class and interface types, like `OwningArray<T>`. A value-typed T written at
// an `OwningTask<T>` instantiation is rejected at that instantiation site (the
// shared `owningElementBadDesc` predicate and the "requires a class or
// interface element type" diagnostic, §3.6 rule 2). An inferred type (a
// `var` initialised from `Domain.Spawn`) is not a written instantiation and
// stays admitted.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The message the shared predicate emits for a value-typed OwningTask element.
const char* kOwningTaskNeedle =
    "`OwningTask` requires a class or interface element type";

}  // namespace

// ACCEPT — an inferred `var` from `Domain.Spawn` is typed OwningTask<int>
// internally but is not a written instantiation: it compiles and runs.
BEZEL_TEST(sema_owning_task_value_type_5701,
           owning_task_inferred_spawn_accepted_spec_14_4) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Count(): int "
        "    return 7; "
        "  end Count; "
        "end C; "
        "class Main "
        "  public static async method Main(): Task<int> "
        "    var j := Domain.Spawn(C.Count); "
        "    return await j; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 7);
}

// ACCEPT — the `using var` form of the inferred spawn is admitted.
BEZEL_TEST(sema_owning_task_value_type_5701,
           owning_task_using_var_spawn_accepted_spec_14_4) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Count(): int "
        "    return 7; "
        "  end Count; "
        "end C; "
        "class Main "
        "  public static async method Main(): Task<int> "
        "    using var j := Domain.Spawn(C.Count); "
        "    return await j; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 7);
}

// ACCEPT — an annotated `Task<int>` (the borrowing form) is not an owning
// instantiation and is admitted.
BEZEL_TEST(sema_owning_task_value_type_5701,
           borrowing_task_int_annotation_accepted_spec_14_4) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static async method Five(): Task<int> "
        "    return 5; "
        "  end Five; "
        "end C; "
        "class Main "
        "  public static async method Main(): Task<int> "
        "    var t: Task<int> := C.Five(); "
        "    return await t; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 5);
}

// REJECT — a written local annotation `var o: OwningTask<int>`.
BEZEL_TEST(sema_owning_task_value_type_5701,
           owning_task_int_local_annotation_rejected_spec_14_4) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Count(): int "
        "    return 7; "
        "  end Count; "
        "end C; "
        "class Main "
        "  public static async method Main(): Task<int> "
        "    var o: OwningTask<int> := Domain.Spawn(C.Count); "
        "    return await o; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kOwningTaskNeedle));
}

// REJECT — a written return type `OwningTask<int>`.
BEZEL_TEST(sema_owning_task_value_type_5701,
           owning_task_int_return_type_rejected_spec_14_4) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Count(): int "
        "    return 7; "
        "  end Count; "
        "  public static method Make(): OwningTask<int> "
        "    return Domain.Spawn(C.Count); "
        "  end Make; "
        "end C; "
        "class Main "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kOwningTaskNeedle));
}

// REJECT — a written field type `OwningTask<int>`.
BEZEL_TEST(sema_owning_task_value_type_5701,
           owning_task_int_field_rejected_spec_14_4) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Count(): int "
        "    return 7; "
        "  end Count; "
        "end C; "
        "class Holder "
        "  private _t: OwningTask<int> := Domain.Spawn(C.Count); "
        "end Holder; "
        "class Main "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kOwningTaskNeedle));
}

// REJECT — a written parameter type `OwningTask<int>`.
BEZEL_TEST(sema_owning_task_value_type_5701,
           owning_task_int_parameter_rejected_spec_14_4) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Take(t: OwningTask<int>): int "
        "    return 0; "
        "  end Take; "
        "end C; "
        "class Main "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kOwningTaskNeedle));
}

// REJECT — a written nullable form `OwningTask<int>?`.
BEZEL_TEST(sema_owning_task_value_type_5701,
           owning_task_int_nullable_rejected_spec_14_4) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Count(): int "
        "    return 7; "
        "  end Count; "
        "end C; "
        "class Main "
        "  public static method Main(): int "
        "    var o: OwningTask<int>? := nothing; "
        "    return 0; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kOwningTaskNeedle));
}

// REJECT — a written generic-argument position `OwningTask<int>` nested in an
// owning collection: the inner written instantiation is itself checked.
BEZEL_TEST(sema_owning_task_value_type_5701,
           owning_task_int_generic_argument_rejected_spec_14_4) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Count(): int "
        "    return 7; "
        "  end Count; "
        "end C; "
        "class Main "
        "  public static method Main(): int "
        "    using var a := new OwningArray<OwningTask<int>>(); "
        "    return 0; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, kOwningTaskNeedle));
}

// ACCEPT — a class element is the admitted form of a written `OwningTask<T>`.
BEZEL_TEST(sema_owning_task_value_type_5701,
           owning_task_class_element_accepted_spec_14_4) {
    auto r = runAot(
        "module M; "
        "sealed class Box "
        "  public method Id(): int "
        "    return 7; "
        "  end Id; "
        "end Box; "
        "class C "
        "  public static method MakeBox(): owned Box "
        "    return new Box(); "
        "  end MakeBox; "
        "end C; "
        "class Main "
        "  public static async method Main(): Task<int> "
        "    var o: OwningTask<Box> := Domain.Spawn(C.MakeBox); "
        "    using var b := await o; "
        "    return b.Id(); "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode == 7);
}
