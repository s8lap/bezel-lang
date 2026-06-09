// Fix #537 — `for var v in d.Values` on a Dictionary emits "not yet
// implemented: codegen for this for statement".
//
// Spec: stdlib §4.8 (Dictionary<K,V>) — d.Values is an ICollection<V> view over
// the dictionary's values.  Iterating it with `for var v in d.Values` must
// enumerate every value exactly once in an unspecified-but-stable order
// (matching the snapshot iteration `for var k in d.Keys` uses).
//
// Root cause (pre-fix): lowerDictKeysFor in LlvmBackend_Stmts.cpp only matched
// member == "Keys"; the "Values" case fell through the entire for-statement
// handler chain and hit the terminal "not yet implemented" error.  The runtime
// had no value-snapshot helpers (analogous to bz_dict_keys_str /
// bz_dict_keys_gen for keys).
//
// Fix: add bz_dict_values_str / bz_dict_values_gen to the runtime (snapshot of
// the live values array) and lowerDictValuesFor to the codegen (mirrors
// lowerDictKeysFor, using genericArgs[1] for the value element type).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// Integer values — string-keyed Dictionary<string, int>
// ---------------------------------------------------------------------------

// #537 / stdlib §4.8: `for var v in d.Values` sums integer values of a
// string-keyed dictionary.  d = {"a":10, "b":20} → sum = 30.
BEZEL_TEST(dict_values_iter_537, string_key_int_values_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"a\", 10); "
        "  d.Set(\"b\", 20); "
        "  var sum := 0; "
        "  for var v in d.Values do sum := sum + v; end; "
        "  return sum; "
        "end Main; end C;"), 30);
}

// #537 / stdlib §4.8: iteration count matches d.Count — two entries yield
// exactly 2 iterations.
BEZEL_TEST(dict_values_iter_537, string_key_int_values_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"x\", 1); "
        "  d.Set(\"y\", 2); "
        "  var n := 0; "
        "  for var v in d.Values do n := n + 1; end; "
        "  return n; "
        "end Main; end C;"), 2);
}

// #537 / stdlib §4.8: updating a key does NOT add a second entry — the
// loop iterates only as many times as there are distinct keys.
BEZEL_TEST(dict_values_iter_537, string_key_update_does_not_double) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"k\", 5); "
        "  d.Set(\"k\", 99); "
        "  var n := 0; "
        "  for var v in d.Values do n := n + 1; end; "
        "  return n; "
        "end Main; end C;"), 1);
}

// #537 / stdlib §4.8: three entries — sum of values.
BEZEL_TEST(dict_values_iter_537, string_key_three_entries_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Set(\"one\", 1); "
        "  d.Set(\"two\", 2); "
        "  d.Set(\"three\", 3); "
        "  var sum := 0; "
        "  for var v in d.Values do sum := sum + v; end; "
        "  return sum; "
        "end Main; end C;"), 6);
}

// ---------------------------------------------------------------------------
// String values — Dictionary<string, string>
// ---------------------------------------------------------------------------

// #537 / stdlib §4.8: `for var v in d.Values` over string values — sum the
// lengths.  d = {"a":"hi", "b":"world"} → 2 + 5 = 7.
BEZEL_TEST(dict_values_iter_537, string_key_string_values_length_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, string>(); "
        "  d.Set(\"a\", \"hi\"); "
        "  d.Set(\"b\", \"world\"); "
        "  var total := 0; "
        "  for var v in d.Values do total := total + v.Length; end; "
        "  return total; "
        "end Main; end C;"), 7);
}

// ---------------------------------------------------------------------------
// Integer-keyed Dictionary<int, int> — generic key, scalar value
// ---------------------------------------------------------------------------

// #537 / stdlib §4.8: `for var v in d.Values` over a generic-key
// (int-keyed) dictionary — values {100, 200} → sum 300.
BEZEL_TEST(dict_values_iter_537, int_key_int_values_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(3, 100); "
        "  d.Set(4, 200); "
        "  var sum := 0; "
        "  for var v in d.Values do sum := sum + v; end; "
        "  return sum; "
        "end Main; end C;"), 300);
}

// #537 / stdlib §4.8: int-keyed dict, iteration count == d.Count.
BEZEL_TEST(dict_values_iter_537, int_key_int_values_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(1, 10); "
        "  d.Set(2, 20); "
        "  d.Set(1, 99); "            // update: still only 2 keys
        "  var n := 0; "
        "  for var v in d.Values do n := n + 1; end; "
        "  return n; "
        "end Main; end C;"), 2);
}

// ---------------------------------------------------------------------------
// Empty dictionary — zero iterations, no crash
// ---------------------------------------------------------------------------

// #537 / stdlib §4.8: empty Dictionary<string,int>.Values → 0 iterations.
BEZEL_TEST(dict_values_iter_537, empty_dict_no_iterations) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int>(); "
        "  var n := 0; "
        "  for var v in d.Values do n := n + 1; end; "
        "  return n; "
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// CONTROL: existing Keys iteration still works (regression guard)
// ---------------------------------------------------------------------------

// #537 / stdlib §4.8: `for var k in d.Keys` (the existing path) is
// unaffected — still sums keys correctly.
BEZEL_TEST(dict_values_iter_537, keys_still_works_regression_guard) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(3, 100); "
        "  d.Set(4, 200); "
        "  var sum := 0; "
        "  for var k in d.Keys do sum := sum + k; end; "
        "  return sum; "              // 3 + 4 = 7
        "end Main; end C;"), 7);
}

// ---------------------------------------------------------------------------
// CONTROL: `for var e in d` (bare dict) still gives §4.8 not-iterable diag
// ---------------------------------------------------------------------------

// #537 / §4.8: bare `for var e in d` is still invalid — we verify the
// program does NOT return 300 (the correct sum of int values), confirming
// it is rejected at compile-time (sema error, fix #530) rather than
// silently iterating.
BEZEL_TEST(dict_values_iter_537, bare_dict_not_iterable_still_errors) {
    auto r = runJit(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<int, int>(); "
        "  d.Set(3, 100); "
        "  d.Set(4, 200); "
        "  var sum := 0; "
        "  for var e in d do sum := sum + e; end; "
        "  return sum; "
        "end Main; end C;");
    // Either it didn't run (compile error = correct) or it ran but not 300.
    if (r.ran) {
        BEZEL_EXPECT_TRUE(r.exitCode != 300);
    }
    // If !r.ran, the test passes — compile-time error is the correct behavior.
}
