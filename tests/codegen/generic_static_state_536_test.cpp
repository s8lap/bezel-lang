// Codegen — generic class static fields are per-instantiation, not shared (issue #536).
//
// §22.12: "each distinct instantiation has its own static state." A static field
// on a generic class `Counter<T>` must have independent storage for
// `Counter<int>` and `Counter<string>` — mutations through one instantiation
// must not affect the other.
//
// Before the fix: static field globals were named `bz$Counter$count` and
// shared across all instantiations. Both `Counter<int>.Increment()` and
// `Counter<string>.Increment()` wrote to the same global, so
// `Counter<int>.Get()` after incrementing only `Counter<int>` returned the
// total across both instantiations.
//
// After the fix: the global is named with the concrete type argument(s),
// e.g. `bz$Counter$int$count` vs `bz$Counter$string$count`, so each
// instantiation has its own independent backing store.
//
// Spec references:
//   §22.12 — static state is per-instantiation.
//   §14.1  — generic classes are monomorphized per type argument.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

bezel::test::JitResult jit(const std::string& prog) {
    return runJit(prog);
}

// A generic counter class whose static field tracks how many times each
// instantiation has been incremented independently.
const char* kCounter =
    "module M; "
    "class Counter<T> "
    "  private static _count: int := 0; "
    "  public static method Increment() _count := _count + 1; end Increment; "
    "  public static method Get(): int return _count; end Get; "
    "end Counter; ";

// A generic class with two static fields.
const char* kBox =
    "module M; "
    "class Box<T> "
    "  private static _val: int := 0; "
    "  private static _extra: int := 10; "
    "  public static method SetVal(n: int) _val := n; end SetVal; "
    "  public static method GetVal(): int return _val; end GetVal; "
    "  public static method GetExtra(): int return _extra; end GetExtra; "
    "end Box; ";

}  // namespace

// ── Core: per-instantiation isolation ────────────────────────────────────────

// §22.12 / #536: Counter<int> incremented ×2 → Get() = 2;
//               Counter<string> incremented ×1 → Get() = 1.
// Both must be independent. Return Counter<int>.Get() (expected 2).
BEZEL_TEST(codegen_generic_static_536, counter_int_is_independent_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kCounter) +
        "class C "
        "  public static method Main(): int "
        "    Counter<int>.Increment(); "
        "    Counter<int>.Increment(); "
        "    Counter<string>.Increment(); "
        "    return Counter<int>.Get(); "
        "  end Main; "
        "end C;"), 2);
}

// §22.12 / #536: same scenario — return Counter<string>.Get() (expected 1).
BEZEL_TEST(codegen_generic_static_536, counter_string_is_independent_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kCounter) +
        "class C "
        "  public static method Main(): int "
        "    Counter<int>.Increment(); "
        "    Counter<int>.Increment(); "
        "    Counter<string>.Increment(); "
        "    return Counter<string>.Get(); "
        "  end Main; "
        "end C;"), 1);
}

// §22.12 / #536: three distinct instantiations (int, string, bool).
// int×3, string×2, bool×1 — verify int count = 3.
BEZEL_TEST(codegen_generic_static_536, three_instantiations_int_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kCounter) +
        "class C "
        "  public static method Main(): int "
        "    Counter<int>.Increment(); "
        "    Counter<int>.Increment(); "
        "    Counter<int>.Increment(); "
        "    Counter<string>.Increment(); "
        "    Counter<string>.Increment(); "
        "    Counter<bool>.Increment(); "
        "    return Counter<int>.Get(); "
        "  end Main; "
        "end C;"), 3);
}

// §22.12 / #536: same — verify string count = 2.
BEZEL_TEST(codegen_generic_static_536, three_instantiations_string_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kCounter) +
        "class C "
        "  public static method Main(): int "
        "    Counter<int>.Increment(); "
        "    Counter<int>.Increment(); "
        "    Counter<int>.Increment(); "
        "    Counter<string>.Increment(); "
        "    Counter<string>.Increment(); "
        "    Counter<bool>.Increment(); "
        "    return Counter<string>.Get(); "
        "  end Main; "
        "end C;"), 2);
}

// §22.12 / #536: same — verify bool count = 1.
BEZEL_TEST(codegen_generic_static_536, three_instantiations_bool_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kCounter) +
        "class C "
        "  public static method Main(): int "
        "    Counter<int>.Increment(); "
        "    Counter<int>.Increment(); "
        "    Counter<int>.Increment(); "
        "    Counter<string>.Increment(); "
        "    Counter<string>.Increment(); "
        "    Counter<bool>.Increment(); "
        "    return Counter<bool>.Get(); "
        "  end Main; "
        "end C;"), 1);
}

// ── Non-default initializer: per-instantiation initialization ────────────────

// §22.12 / #536: a generic static field initialized to a non-zero value.
// Box<int>._extra starts at 10 and Box<long>._extra starts at 10 independently.
// After SetVal(7) on Box<int>, GetVal() = 7 for <int> and 0 for <long>.
BEZEL_TEST(codegen_generic_static_536, nondefault_initializer_int_vs_long_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kBox) +
        "class C "
        "  public static method Main(): int "
        "    Box<int>.SetVal(7); "
        "    var intVal := Box<int>.GetVal(); "
        "    var longVal := Box<long>.GetVal(); "
        "    if intVal = 7 and longVal = 0 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// §22.12 / #536: _extra (initialized to 10) must be independent too.
// Box<int> and Box<string> each see their own copy of _extra = 10.
BEZEL_TEST(codegen_generic_static_536, two_static_fields_both_independent_22_12) {
    BEZEL_EXPECT_EQ(run(
        std::string(kBox) +
        "class C "
        "  public static method Main(): int "
        "    var intExtra := Box<int>.GetExtra(); "
        "    var strExtra := Box<string>.GetExtra(); "
        "    if intExtra = 10 and strExtra = 10 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// ── Control: non-generic static field still works ─────────────────────────────

// §22 / #536 regression guard: a plain non-generic static field must still
// accumulate normally. This verifies the base CG12 path is unaffected.
BEZEL_TEST(codegen_generic_static_536, control_nongeneric_static_field_22) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter "
        "  private static _count: int := 0; "
        "  public static method Increment() _count := _count + 1; end Increment; "
        "  public static method Get(): int return _count; end Get; "
        "end Counter; "
        "class C "
        "  public static method Main(): int "
        "    Counter.Increment(); Counter.Increment(); Counter.Increment(); "
        "    return Counter.Get(); "
        "  end Main; "
        "end C;"), 3);
}

// ── Control: #520 generic static METHOD still works ───────────────────────────

// §14.1 / §22.12 / #520 regression guard: generic static methods that only
// use parameters (not static fields) must still return the correct value.
BEZEL_TEST(codegen_generic_static_536, control_generic_static_method_520_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Registry<T> "
        "  public static method Make(v: T): T "
        "    return v; "
        "  end Make; "
        "end Registry; "
        "class C "
        "  public static method Main(): int "
        "    return Registry<int>.Make(42); "
        "  end Main; "
        "end C;"), 42);
}

// ── Control: static const on a generic class ─────────────────────────────────

// §22.12 / #536: a static const (literal initializer, never written) on a
// generic class still returns its constant value for each instantiation.
BEZEL_TEST(codegen_generic_static_536, control_static_const_generic_class_22_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Tagged<T> "
        "  public static Version: int := 99; "
        "  public static method GetVersion(): int return Version; end GetVersion; "
        "end Tagged; "
        "class C "
        "  public static method Main(): int "
        "    var a := Tagged<int>.GetVersion(); "
        "    var b := Tagged<string>.GetVersion(); "
        "    if a = 99 and b = 99 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// ── No compile errors for valid per-instantiation static field use ────────────

// §22.12 / #536: no compile errors for well-formed code using generic static fields.
BEZEL_TEST(codegen_generic_static_536, no_error_for_valid_code_22_12) {
    auto r = jit(
        std::string(kCounter) +
        "class C "
        "  public static method Main(): int "
        "    Counter<int>.Increment(); "
        "    return Counter<int>.Get(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ── §22.12 / §28.5 / #1116: static `owned T?` field in a class-constrained ──
// generic class must use the CONCRETE LLVM type (ptr) for each per-instantiation
// global — not the placeholder {i8,i32} nullable-value-struct that declareStatics
// assigns to the unresolved `T?` template field.  Before the fix, storing a class
// reference into a {i8,i32} global then loading it as {i8,i32} silently
// miscompiled: the null-check read the pointer's low byte as the hasValue tag and
// the unwrapped "value" was a 32-bit fragment of the address — Val() returned 0.

// Minimal repro: `Holder<Cell>.shared := new Cell(4)` followed by
// `Holder<Cell>.shared!.Val()` must return 4.
// Per §22.12 (per-instantiation static state) / §28.5 (owned fields).
BEZEL_TEST(codegen_generic_static_536, static_owned_typeParam_nullable_field_roundtrip_22_12_28_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Val(): int => _v; "
        "end Cell; "
        "class Holder<T> where T: class "
        "  public static owned shared: T?; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    Holder<Cell>.shared := new Cell(4); "
        "    return Holder<Cell>.shared!.Val(); "
        "  end Main; "
        "end C;"), 4);
}

// Two distinct instantiations of the `static owned T?` field must have
// independent storage and independent concrete types.  Holder<Cell>.shared and
// Holder<Dog>.shared must not alias each other's per-instantiation global.
// Per §22.12 (each instantiation has its own static state) / §28.5 / #1116.
BEZEL_TEST(codegen_generic_static_536, static_owned_typeParam_nullable_two_instantiations_22_12_28_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Val(): int => _v; "
        "end Cell; "
        "class Dog "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Val(): int => _v; "
        "end Dog; "
        "class Holder<T> where T: class "
        "  public static owned shared: T?; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    Holder<Cell>.shared := new Cell(10); "
        "    Holder<Dog>.shared := new Dog(20); "
        "    var a := Holder<Cell>.shared!.Val(); "
        "    var b := Holder<Dog>.shared!.Val(); "
        "    return a + b; "
        "  end Main; "
        "end C;"), 30);
}

// Concrete non-generic static owned field on a non-generic class must still work
// after the #1116 change (no regression on the non-generic path).
// Per §22.8 (static fields) / §28.5 (owned fields).
// Updated for §22.12 rule 2 (v0.163, #1363): a mutable static class field is a
// compile error; the field uses `static owned readonly` with a static initializer
// instead — exercising the same ownership/disposal path (§28.5) under rule 4
// (warning-only, not an error, for static readonly class fields).
BEZEL_TEST(codegen_generic_static_536, concrete_static_owned_field_unaffected_22_8_28_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Val(): int => _v; "
        "end Cell; "
        "class Store "
        "  public static owned readonly item: Cell := new Cell(42); "
        "end Store; "
        "class C "
        "  public static method Main(): int "
        "    return Store.item.Val(); "
        "  end Main; "
        "end C;"), 42);
}
