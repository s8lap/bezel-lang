// fix #766 / §14.1: instantiating a generic record whose type argument is itself
// a bare type parameter of an enclosing scope — `new R<T>(...)` inside a generic
// `Make<T>()` — resolves the argument to the concrete type, not the unresolved
// `T`.
//
// `record R<T>(V1: int, V2: T?)`, `Make<T>(): R<T>` called as `Make<int>`: inside
// Make, R is instantiated with R's `T` argument bound to Make's `T`, which is
// bound to `int` at the call site. The bug: the inner record's sema-type
// substitution map stored the UNRESOLVED `T`, so the field layout used the i32
// placeholder while the `V2: T?` value is the 8-byte `{i8 hasValue; int value}`
// struct — an 8-into-4 stack write (SIGSEGV). The fix resolves each generic
// argument through the active outer substitution before building the inner map.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §14.1: the #766 repro — `Make<int>()` returning `new R<T>(0, default(T?))`
// no longer crashes (it SIGSEGV'd at the field store).
BEZEL_TEST(generic_record_through_method_766, instantiation_does_not_crash) {
    BEZEL_EXPECT_EQ(runFull(
        "module TestGen; "
        "record R<T>(V1: int, V2: T?); "
        "static class F "
        "  public static method Make<T>(): R<T> "
        "    return new R<T>(0, default(T?)); "
        "  end Make; "
        "end F; "
        "public static class Program "
        "  public static method Main(): int "
        "    var r: R<int> := F.Make<int>(); "
        "    return 0; "
        "  end Main; "
        "end Program;"), 0);
}

// §14.1: the fields read back correctly through the generic-method instantiation
// — V1 (non-generic int) and V2 (the generic T? value). 10 + 32 = 42.
BEZEL_TEST(generic_record_through_method_766, fields_read_back) {
    BEZEL_EXPECT_EQ(runFull(
        "module TestGen; "
        "record R<T>(V1: int, V2: T?); "
        "static class F "
        "  public static method Make<T>(v1: int, v2: T): R<T> "
        "    return new R<T>(v1, v2); "
        "  end Make; "
        "end F; "
        "public static class Program "
        "  public static method Main(): int "
        "    var r := F.Make<int>(10, 32); "
        "    if r.V2 = null then return 9; end; "
        "    return r.V1 + r.V2!; "  // 42
        "  end Main; "
        "end Program;"), 42);
}

// §14.1: a default(T?) inside the generic method yields a genuinely-null value
// field on the concrete instantiation.
BEZEL_TEST(generic_record_through_method_766, default_yields_null) {
    BEZEL_EXPECT_EQ(runFull(
        "module TestGen; "
        "record R<T>(V1: int, V2: T?); "
        "static class F "
        "  public static method Make<T>(): R<T> "
        "    return new R<T>(7, default(T?)); "
        "  end Make; "
        "end F; "
        "public static class Program "
        "  public static method Main(): int "
        "    var r := F.Make<int>(); "
        "    return 0 if r.V2 = null else 1; "
        "  end Main; "
        "end Program;"), 0);
}
