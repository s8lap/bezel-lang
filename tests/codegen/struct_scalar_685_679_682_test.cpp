// Codegen tests for fix #685, #679, #682: struct-as-scalar lowering family.
//
// Root family: collection/field/index lowerings that assumed every value is a
// scalar integer or pointer.  Five confirmed shapes:
//   #679: field-stored Mutex/Lock passed as i32 to bz_mutex_acquire (lowerTypeRef
//         missing "Mutex"/"Lock" in the ptrTy name list).
//   #682: arr[2..5] range-slice → SExt on %bz.range struct (isArr path called
//         CreateSExt on a range struct index instead of dispatching to bz_array_slice).
//   #685a: Set<decimal>.Add/Contains → ICmpInst SIGABRT (equality emitted ICmpEQ
//          on %bz.decimal struct; must route bz_decimal_cmp).
//   #685b: Dictionary<string,decimal>.Add → ZExt IR-fail (decimal value not
//          heap-boxed by encodeValue).
//   #685c: Dictionary<decimal,int>.Add/lookup → 'Load operand must be a pointer'
//          (decimal key hashed/loaded as bz_string pointer via string fast-path).
//
// Spec: §4.4 (decimal value type), §6.2 (Mutex/Lock), §8.3 (hash-equality contract),
//       §15.6 (array slicing), stdlib §4.8 (Dictionary), stdlib §4.10 (Set).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// #679: field-stored Mutex — Acquire() must not ICE (was: i32-vs-ptr IR fail)
// Spec: §6.2 / stdlib §6.2: Mutex is a heap-allocated builtin; lowerTypeRef must
// return ptrTy() for a Mutex or Lock field so bz_mutex_acquire receives a ptr.
// ---------------------------------------------------------------------------

// #679 / §6.2: field-stored Mutex; Acquire/Dispose cycle must not ICE.
// The primary fix is IR-level: Mutex field must lower as ptr, not i32, so
// bz_mutex_acquire receives a valid pointer argument.
BEZEL_TEST(struct_scalar_679, field_mutex_acquire_dispose) {
    // #679 fix: Mutex field must lower as ptr (not i32); pre-fix, lowerTypeRef
    // returned i32 for "Mutex", causing an IR-verify fail (bz_mutex_acquire
    // was called with i32 argument). After fix, ran=true AND Balance=42.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class BankAccount "
        "  private owned _mutex: Mutex := new Mutex(); "
        "  private _balance: int := 0; "
        "  public method Deposit(amount: int) "
        "    using var guard := _mutex.Acquire(); "
        "    _balance := _balance + amount; "
        "  end Deposit; "
        "  public property Balance: int => _balance; "
        "  public static method Main(): int "
        "    using var acct := new BankAccount(); "
        "    acct.Deposit(42); "
        "    return acct.Balance; "
        "  end Main; "
        "end BankAccount;"), 42);
}

// #679 / §6.2: field-stored Mutex with decimal field (both struct-typed fields
// in the same class — exercises the field layout for multiple struct types).
BEZEL_TEST(struct_scalar_679, field_mutex_with_decimal_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Acct "
        "  private owned _mutex: Mutex := new Mutex(); "
        "  private _balance: decimal := 0m; "
        "  public method Deposit(amount: decimal) "
        "    using var guard := _mutex.Acquire(); "
        "    _balance := _balance + amount; "
        "  end Deposit; "
        "  public static method Main(): int "
        "    using var acct := new Acct(); "
        "    acct.Deposit(100m); "
        "    return 42; "
        "  end Main; "
        "end Acct;"), 42);
}

// ---------------------------------------------------------------------------
// #682: range-slice arr[2..5] — must not ICE (SExt on struct was the bug)
// Spec: §15.6: arr[a..b] produces a new Array<T> containing copies of [a,b].
//
// Sema currently types `arr[range]` as the element type T (same path as
// scalar indexer — §15.6 sema support for typed-slice result is tracked as a
// follow-on). These tests verify the primary fix: the codegen path detects the
// range index and emits bz_array_slice rather than attempting to SExt a
// %bz.range struct (which was the original ICE). Correctness tests (Count,
// element values) require the sema side; they are filed separately.
// ---------------------------------------------------------------------------

// #682 / §15.6: arr[2..5] inclusive range — must compile to valid IR (no ICE).
// Pre-fix: CreateSExt on %bz.range struct → IR-verify SIGABRT.
// Post-fix: detects range index, calls bz_array_slice, returns ptr.
BEZEL_TEST(struct_scalar_682, array_range_slice_inclusive_no_ice) {
    // Verify the JIT ran (ran=true) — no IR-verification failure.
    // The return value of s.Count is not checked here because sema types
    // arr[range] as the element type (T=int), so s.Count lowers as int.Count
    // which returns a default 0.  The IR-level fix is what matters for #682.
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(0); arr.Add(1); arr.Add(2); arr.Add(3); "
        "    arr.Add(4); arr.Add(5); arr.Add(6); arr.Add(7); "
        "    using var s := arr[2..5]; "
        "    return 42; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 42);
}

// #682 / §15.6: arr[1..3] inclusive — no ICE, compiles + runs.
BEZEL_TEST(struct_scalar_682, array_range_slice_values_no_ice) {
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); arr.Add(20); arr.Add(30); arr.Add(40); arr.Add(50); "
        "    using var s := arr[1..3]; "
        "    return 42; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 42);
}

// #682 / §15.6: arr[1..<4] half-open — no ICE, compiles + runs.
BEZEL_TEST(struct_scalar_682, array_range_slice_half_open_no_ice) {
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); arr.Add(3); arr.Add(4); arr.Add(5); arr.Add(6); "
        "    using var s := arr[1..<4]; "
        "    return 42; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 42);
}

// #682 / §15.6: out-of-range slice arr[3..2] (start > end) — bounds-check fires.
// The program throws (unhandled exception → exit 70); we just verify the IR
// was valid (ran=true, no IR-verification SIGABRT / -999).
BEZEL_TEST(struct_scalar_682, array_range_slice_oor_no_ice) {
    auto r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); arr.Add(3); arr.Add(4); arr.Add(5); "
        "    using var s := arr[3..2]; "
        "    return 0; "
        "  end Main; "
        "end C;");
    // ran=true means IR was valid (bz_array_slice was called, bounds guard fired).
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// #685a: Set<decimal> — Add, Contains, equality — was SIGABRT from ICmpEQ on struct
// Spec: §4.4 (decimal), §8.3 (hash-equality), stdlib §4.10 (Set<T>)
// ---------------------------------------------------------------------------

// #685a / §4.4 / §8.3 / stdlib §4.10: Set<decimal>.Contains after Add.
BEZEL_TEST(struct_scalar_685a, set_decimal_add_contains) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var s := new Set<decimal>(); "
        "    s.Add(3.14159m); "
        "    s.Add(2.71828m); "
        "    if s.Contains(3.14159m) then "
        "      return 42; "
        "    end; "
        "    return 1; "
        "  end Main; "
        "end C;"), 42);
}

// #685a / §4.4 / §8.3 / stdlib §4.10: Set<decimal> dedup — duplicate elements
// are not stored twice.
BEZEL_TEST(struct_scalar_685a, set_decimal_dedup) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var s := new Set<decimal>(); "
        "    s.Add(1.5m); s.Add(1.5m); s.Add(2.0m); "
        "    return s.Count; "
        "  end Main; "
        "end C;"), 2);
}

// #685a / §4.4 / §8.3: normalized equality — 1.0m and 1.00m must be deduplicated
// (bz_decimal_hash normalizes trailing zeros so they hash identically,
// consistent with bz_decimal_cmp equality).
BEZEL_TEST(struct_scalar_685a, set_decimal_normalized_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var s := new Set<decimal>(); "
        "    s.Add(1.0m); "
        "    s.Add(1.00m); "
        "    return s.Count; "  // 1.0m == 1.00m → one distinct element
        "  end Main; "
        "end C;"), 1);
}

// #685a / §4.4 / stdlib §4.10: Set<decimal>.Contains with normalized form.
BEZEL_TEST(struct_scalar_685a, set_decimal_contains_normalized) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var s := new Set<decimal>(); "
        "    s.Add(1.0m); "
        "    if s.Contains(1.00m) then return 42; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 42);
}

// ---------------------------------------------------------------------------
// #685b: Dictionary<string, decimal> value — was ZExt IR-fail
// Spec: §4.4, stdlib §4.8 (Dictionary value slot ABI for struct-typed values)
// ---------------------------------------------------------------------------

// #685b / §4.4 / stdlib §4.8: Dict<string,decimal>.Add does not crash.
BEZEL_TEST(struct_scalar_685b, dict_string_decimal_add) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, decimal>(); "
        "    d.Add(\"x\", 1.5m); "
        "    return 42; "
        "  end Main; "
        "end C;"), 42);
}

// #685b / §4.4 / stdlib §4.8: Dict<string,decimal> round-trip (TryGetValue).
BEZEL_TEST(struct_scalar_685b, dict_string_decimal_trygetvalue) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, decimal>(); "
        "    d.Add(\"pi\", 3.14m); "
        "    var v: decimal? := null; "  // v0.171 (#1336): out is V?
        "    d.TryGetValue(\"pi\", out v); "
        "    if v = 3.14m then return 42; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 42);
}

// #685b / §4.4 / stdlib §4.8: Dict<string,decimal> with multiple entries.
BEZEL_TEST(struct_scalar_685b, dict_string_decimal_multiple) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, decimal>(); "
        "    d.Add(\"a\", 1.1m); d.Add(\"b\", 2.2m); d.Add(\"c\", 3.3m); "
        "    return d.Count; "
        "  end Main; "
        "end C;"), 3);
}

// ---------------------------------------------------------------------------
// #685c: Dictionary<decimal, int> key — was 'Load operand must be a pointer'
// Spec: §4.4, §8.3, stdlib §4.8 (decimal key — must use generic-key hash path)
// ---------------------------------------------------------------------------

// #685c / §4.4 / §8.3 / stdlib §4.8: Dict<decimal,int>.Add does not crash.
BEZEL_TEST(struct_scalar_685c, dict_decimal_key_add) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<decimal, int>(); "
        "    d.Add(1.5m, 42); "
        "    return 42; "
        "  end Main; "
        "end C;"), 42);
}

// #685c / §4.4 / §8.3 / stdlib §4.8: Dict<decimal,int> round-trip (ContainsKey).
BEZEL_TEST(struct_scalar_685c, dict_decimal_key_containskey) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<decimal, int>(); "
        "    d.Add(3.14m, 1); d.Add(2.71m, 2); "
        "    if d.ContainsKey(3.14m) then return 42; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 42);
}

// #685c / §4.4 / §8.3 / stdlib §4.8: decimal-key dict TryGetValue round-trip.
BEZEL_TEST(struct_scalar_685c, dict_decimal_key_trygetvalue) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<decimal, int>(); "
        "    d.Add(10.5m, 99); "
        "    var v: int? := null; "  // v0.171 (#1336): out is V?
        "    d.TryGetValue(10.5m, out v); "
        "    return v!; "
        "  end Main; "
        "end C;"), 99);
}

// #685c / §4.4 / §8.3: normalized decimal key — 1.0m and 1.00m must resolve to
// the same entry (same normalization rule as Set<decimal>).
BEZEL_TEST(struct_scalar_685c, dict_decimal_key_normalized_lookup) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<decimal, int>(); "
        "    d.Add(1.0m, 55); "
        "    var v: int? := null; "  // v0.171 (#1336): out is V?
        "    d.TryGetValue(1.00m, out v); "
        "    return v!; "
        "  end Main; "
        "end C;"), 55);
}
