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

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

bezel::test::JitResult jit(const std::string& prog) {
    return runAot(prog);
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

// #2331 / §22.12 rule 2 (v0.230): a mutable (non-`readonly`, non-`const`)
// static field or auto-property whose declared type is a bare generic
// parameter of the enclosing TYPE (T, T?) is admitted by no form above and
// is a COMPILE ERROR at the declaration site, whatever the parameter's
// constraints or eventual substitutions. The original spec cited the two
// tests below as roundtrip cases; #1393's first implementation routed this
// through rule 2's residual error branch and regressed them, then `adff878`
// restored status quo pending the v0.230 ruling. v0.230 settles the case
// as a declaration-site rejection; the roundtrip shape the v0.230 comment
// preserved flips to a rejection case — the §22.12 ruling text states the
// case is "admitted by no form above" — and the per-instantiation roundtrip
// behaviour moves to the `static owned readonly` form (the v0.262 ruling
// #3269 / issue #3272 classifies the readonly arm per instantiation).
//
// The fix: emit a declaration-site compile error here; pin the §22.12 /
// #2331 rejection in the same tests. (Per-lane instruction: do NOT close
// #2331 in this commit; the change here ships the ruling's enforcement and
// the comment cites both issues. #2331 stays open as the issue tracker
// entry until closed in a separate commit.)
BEZEL_TEST(codegen_generic_static_536, mutable_typeParam_static_field_rejected_spec_22_12_v0_230_2331) {
    auto r = jit(
        "module M; "
        "class Cell "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Val(): int => _v; "
        "end Cell; "
        "class Holder<T> where T: class "
        "  public static shared: T?; "  // mutable — rejected at declaration
        "end Holder; "
        "class C "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found22_12 = false, found2331 = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§22.12") != std::string::npos) found22_12 = true;
        if (d.message.find("#2331") != std::string::npos) found2331 = true;
    }
    BEZEL_EXPECT_TRUE(found22_12);
    BEZEL_EXPECT_TRUE(found2331);
}

// Companion to the rejection case above: the two-instantiation shape that
// the original roundtrip pinned is now rejected at the declaration for the
// same reason (one diagnostic, not two — the rejection is keyed to the
// declaration, not to each instantiation).
BEZEL_TEST(codegen_generic_static_536, mutable_typeParam_static_two_insts_one_rejection_spec_22_12_v0_230_2331) {
    auto r = jit(
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
        "  public static shared: T?; "  // mutable — rejected at declaration
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h1 := new Holder<Cell>(); "
        "    var h2 := new Holder<Dog>(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    int rejectionCount = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("§22.12 rule 2") != std::string::npos &&
            d.message.find("#2331") != std::string::npos) {
            ++rejectionCount;
        }
    }
    BEZEL_EXPECT_EQ(rejectionCount, 1);
}

// Per-instantiation CONTROL via the READONLY arm — the v0.262 ruling
// (#3269 / issue #3272) classifies `static owned readonly F: T?;` per
// instantiation: a class-typed instantiation takes rule 4's warning unless
// the substituted class is in the warning-suppression set. `Mutex` is in
// the form-(i) synchronized set which silences rule 4 — so
// `Holder<Mutex>` instantiates warning-free. Note that the `readonly`
// modifier prevents an external assign (§22.8), so the roundtrip-control
// shape (assign then read) that the v0.230 mutable arm used is
// unavailable here; this test pins the resolution-only path (the warning
// is suppressed; the declaration is admitted per-instantiation). The
// §28.5 disposal hook is exercised by the v0.230 mutable case above
// (which now rejects at the declaration per #2331) and by the
// non-generic `Store.item` control — together they cover the static-
// `owned` disposal path.
BEZEL_TEST(codegen_generic_static_536, readonly_typeParam_mutex_no_warning_spec_22_12_v0_262_3272) {
    auto r = jit(
        "module M; "
        "class Holder<T> where T: class "
        "  public static owned readonly shared: T?; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var s: Mutex? := Holder<Mutex>.shared; "
        "    if s = null then return 0; else return 1; end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
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
