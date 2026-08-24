// Full-pipeline integration tests: drive the real compiler (front-end
// incl. ownership → LLVM object → clang link), run the produced native
// binary, and check its exit code. Unlike the JIT feature tests (which
// skip ownership/definite-assignment), these exercise the entire pipeline
// on ownership-clean programs, so they validate that real binaries of the
// headline features actually build and run.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirInteg() {
    return wtScratchDir("wt-integ");
}

int compileAndRun(const std::string& source) {
    auto dir = wtDirInteg();
    std::string stem = unique_stem("bezel_integ");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

} // namespace

BEZEL_TEST(integration, virtual_dispatch_through_base_param) {
    // A Base-typed parameter holding a Square dispatches to the override.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Shape public virtual method Area(): int => 0; end Shape; "
        "class Square : Shape public override method Area(): int => 16; "
        "end Square; "
        "class C "
        "  public static method AreaOf(s: Shape): int => s.Area(); "
        "  public static method Main(): int "
        "    using var sq := new Square(); return AreaOf(sq); "
        "  end Main; "
        "end C;"), 16);
}

BEZEL_TEST(integration, cross_method_exception_propagation) {
    // A callee throws; the caller's try catches it after the call unwinds.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class AppError: Exception end AppError; class C "
        "  public static method Risky(n: int): int "
        "    if n < 0 then throw new AppError(); end; return n * 2; "
        "  end Risky; "
        "  public static method Main(): int "
        "    try return Risky(0 - 1); catch AppError => return 99; end; "
        "  end Main; "
        "end C;"), 99);
}

BEZEL_TEST(integration, overflow_checked_and_caught) {
    // A real binary raises and catches OverflowException.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var a := 2000000000; "
        "  try var r := a + a; return r; "
        "  catch OverflowException => return 7; end; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(integration, disposal_via_destructor_counter) {
    // A using var's destructor runs at scope end (a do-block), bumping a
    // static the program then returns.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "  public static method Main(): int "
        "    do using var r := new R(); end; "
        "    return _n; "
        "  end Main; "
        "end R;"), 1);
}

BEZEL_TEST(integration, case_expression) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var x := 2; "
        "  var r := (case x of 1 => 1, 2 => 22, _ => 0); "
        "  return r; "
        "end Main; end C;"), 22);
}

BEZEL_TEST(integration, for_in_range_sum) {
    // for var i in lo..hi counted loop (§16.5) in a real native binary.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var s := 0; for var i in 1..10 do s := s + i; end; return s; "
        "end Main; end C;"), 55);
}

BEZEL_TEST(integration, for_in_range_step) {
    // for with step clause (§16.4): ascending by 2, sum = 0+2+4+6+8+10 = 30.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var s := 0; for var i in 0..10 step 2 do s := s + i; end; return s; "
        "end Main; end C;"), 30);
}

BEZEL_TEST(integration, record_structural_equality) {
    // Record = / <> (§8.3) — two identical records are equal; modified is not.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a := new Point(3, 4); "
        "  var b := new Point(3, 4); "
        "  var c := a with(x := 9); "
        "  return (if a = b then 1 else 0) + (if a <> c then 1 else 0); "
        "end Main; end C;"), 2);
}

BEZEL_TEST(integration, for_with_break_and_sum) {
    // break inside for-in exits early; values before the break are counted.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var s := 0; "
        "  for var i in 1..100 do "
        "    if i > 5 then break; end; "
        "    s := s + i; "
        "  end; "
        "  return s; "
        "end Main; end C;"), 15);
}

BEZEL_TEST(integration, fibonacci_via_for_in) {
    // Fibonacci using for-in — integration test for combined control flow.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var a := 0; var b := 1; "
        "  for var i in 0..9 do var c := a + b; a := b; b := c; end; "
        "  return a; "
        "end Main; end C;"), 55);
}

BEZEL_TEST(integration, sieve_of_eratosthenes) {
    // Sieve of Eratosthenes using an inline [50]int array.
    // Counts primes < 50 (2,3,5,7,11,13,17,19,23,29,31,37,41,43,47 = 15).
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var sieve: [50]int; "
        "  for var i in 0..49 do sieve[i] := 1; end; "
        "  sieve[0] := 0; sieve[1] := 0; "
        "  for var i in 2..49 do "
        "    if sieve[i] = 1 then "
        "      var j := i * 2; "
        "      while j <= 49 do sieve[j] := 0; j := j + i; end; "
        "    end; "
        "  end; "
        "  var count := 0; "
        "  for var i in 0..49 do if sieve[i] = 1 then count := count + 1; end; end; "
        "  return count; "
        "end Main; end C;"), 15);
}

BEZEL_TEST(integration, bubble_sort) {
    // Bubble sort a 5-element inline array — tests array element swaps.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var a: [5]int; "
        "  a[0] := 3; a[1] := 1; a[2] := 4; a[3] := 1; a[4] := 5; "
        "  for var i in 0..3 do "
        "    for var j in 0..3 do "
        "      if a[j] > a[j+1] then "
        "        var t := a[j]; a[j] := a[j+1]; a[j+1] := t; "
        "      end; "
        "    end; "
        "  end; "
        "  var s := 0; for var i in 0..4 do s := s + a[i]; end; return s; "
        "end Main; end C;"), 14);  // [3,1,4,1,5] sorted = [1,1,3,4,5], sum=14
}

BEZEL_TEST(integration, grade_classifier_with_range_patterns) {
    // Records + range patterns in case + for-in + inline arrays.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; record Score(value: int); "
        "class C "
        "  public static method Grade(s: Score): int "
        "    return (case s.value of "
        "      90..100 => 4, 80..89 => 3, 70..79 => 2, "
        "      60..69 => 1, _ => 0); "
        "  end Grade; "
        "  public static method Main(): int "
        "    var scores: [5]int; "
        "    scores[0] := 95; scores[1] := 85; scores[2] := 75; "
        "    scores[3] := 65; scores[4] := 55; "
        "    var s := 0; "
        "    for var i in 0..4 do "
        "      s := s + Grade(new Score(scores[i])); "
        "    end; "
        "    return s; "
        "  end Main; "
        "end C;"), 10);
}

BEZEL_TEST(integration, for_in_array_with_records) {
    // for-in over array of records — iterates element values.
    // [P(1,2), P(3,4), P(5,6)] sums to (1+2)+(3+4)+(5+6) = 21.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a: [3]P; "
        "  a[0] := new P(1, 2); a[1] := new P(3, 4); a[2] := new P(5, 6); "
        "  var s := 0; for var p in a do s := s + p.x + p.y; end; "
        "  return s; "
        "end Main; end C;"), 21);
}

BEZEL_TEST(integration, in_range_operator_filter) {
    // `x in lo..hi` as a filter in a for-in-array loop.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var a: [10]int; "
        "  for var i in 0..9 do a[i] := i * 10; end; "
        "  var cnt := 0; "
        "  for var x in a do if x in 20..60 then cnt := cnt + 1; end; end; "
        "  return cnt; "
        "end Main; end C;"), 5);  // 20,30,40,50,60 = 5 elements
}

BEZEL_TEST(integration, range_case_pattern_classification) {
    // Range patterns in case expression over values computed in for loop.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var a: [5]int; "
        "  a[0] := 95; a[1] := 85; a[2] := 75; a[3] := 65; a[4] := 55; "
        "  var s := 0; "
        "  for var x in a do "
        "    var g := (case x of 90..100 => 4, 80..89 => 3, 70..79 => 2, 60..69 => 1, _ => 0); "
        "    s := s + g; "
        "  end; "
        "  return s; "
        "end Main; end C;"), 10);  // 4+3+2+1+0 = 10 (fits in exit code)
}

BEZEL_TEST(integration, console_writeline_sum) {
    // Console.WriteLine with int — prints the sum and returns it.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var s := 0; "
        "  for var i in 1..10 do s := s + i; end; "
        "  Console.WriteLine(s); "
        "  return s; "
        "end Main; end C;"), 55);
}

BEZEL_TEST(integration, string_interpolation_result) {
    // Interpolated string: construct and measure length.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var x := 42; var s := $\"x={x}\"; "
        "  return s.Length; "
        "end Main; end C;"), 4);  // "x=42" = 4 chars
}

BEZEL_TEST(integration, string_for_in_and_count) {
    // for-in over string counting a specific char (native path).
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var s := \"mississippi\"; var cnt := 0; "
        "  for var c in s do if c = 's' then cnt := cnt + 1; end; end; "
        "  return cnt; "
        "end Main; end C;"), 4);  // 's' appears 4 times
}

BEZEL_TEST(integration, string_methods_chain) {
    // StartsWith + Substring chain — verify start and extract.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  if s.StartsWith(\"Hello\") then "
        "    var rest := s.Substring(6); "
        "    return rest.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 5);  // "World" has length 5
}

BEZEL_TEST(integration, fizzbuzz_via_console) {
    // FizzBuzz 1..15 — tests Console.WriteLine(int), string, + conditional logic.
    // Check exit code (must be 0 = success).
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  for var i in 1..15 do "
        "    if i mod 15 = 0 then Console.WriteLine(\"FizzBuzz\"); "
        "    elif i mod 3 = 0 then Console.WriteLine(\"Fizz\"); "
        "    elif i mod 5 = 0 then Console.WriteLine(\"Buzz\"); "
        "    else Console.WriteLine(i); end; "
        "  end; return 0; "
        "end Main; end C;"), 0);
}

BEZEL_TEST(integration, for_in_string_char_count) {
    // Count occurrences of 's' in "mississippi" via for-in-string.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var s := \"mississippi\"; var cnt := 0; "
        "  for var c in s do if c = 's' then cnt := cnt + 1; end; end; "
        "  return cnt; "
        "end Main; end C;"), 4);
}

BEZEL_TEST(integration, string_isnotempty_branch) {
    // string.IsNullOrEmpty gate (non-empty path).
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var s := \"hello\"; "
        "  if string.IsNullOrEmpty(s) then return 0; end; "
        "  return s.Length; "
        "end Main; end C;"), 5);
}

BEZEL_TEST(integration, string_isempty_branch) {
    // string.IsNullOrEmpty gate (empty path → true). Exercises the branch whose
    // PHI edge supplies `true`; a dead, unterminated `ine.null` block (malformed
    // IR, removed) used to sit alongside it. Empty string → true → return 7.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var s := \"\"; "
        "  if string.IsNullOrEmpty(s) then return 7; end; "
        "  return s.Length; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(integration, string_replace_and_case) {
    // Replace + ToUpper chain.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var s := \"hello world\"; "
        "  var t := s.Replace(\"world\", \"bezel\").ToUpper(); "
        "  return (if t = \"HELLO BEZEL\" then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(integration, math_pow_and_sqrt) {
    // Math.Pow and Math.Sqrt integration.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var area := Math.PI * Math.Pow(5.0, 2.0); "
        "  return (if area > 78.0 then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(integration, recursive_fibonacci) {
    // Fibonacci via recursive method call.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static method Fib(n: int): int "
        "    if n <= 1 then return n; end; "
        "    return Fib(n - 1) + Fib(n - 2); "
        "  end Fib; "
        "  public static method Main(): int return Fib(10); "
        "  end Main; "
        "end C;"), 55);
}

BEZEL_TEST(integration, exception_catch_app_error) {
    // User-defined exception type caught by its exact type.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class AppError: Exception end AppError; "
        "class C "
        "  public static method Main(): int "
        "    try throw new AppError(); "
        "    catch AppError => return 7; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 7);
}

BEZEL_TEST(integration, string_interpolation_with_class) {
    // Interpolated string with class {obj} calls ToString().
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Point "
        "  public _x: int := 0; public _y: int := 0; "
        "  public override method ToString(): string "
        "    return $\"({_x},{_y})\"; "
        "  end ToString; "
        "end Point; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new Point(); "
        "    p._x := 3; p._y := 4; "
        "    var s := $\"Point: {p}\"; "
        "    return s.Length; "
        "  end Main; "
        "end C;"), 12);  // "Point: (3,4)" = 12 chars
}

BEZEL_TEST(integration, sieve_of_eratosthenes_with_output) {
    // Sieve producing sum and count of primes ≤ 50.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var sieve: [51]int; "
        "  for var i in 0..50 do sieve[i] := 1; end; "
        "  sieve[0] := 0; sieve[1] := 0; "
        "  for var i in 2..50 do "
        "    if sieve[i] = 1 then "
        "      var j := i * 2; "
        "      while j <= 50 do sieve[j] := 0; j := j + i; end; "
        "    end; "
        "  end; "
        "  var count := 0; "
        "  for var i in 2..50 do if sieve[i] = 1 then count := count + 1; end; end; "
        "  return count; "
        "end Main; end C;"), 15);  // primes ≤ 50: 15 total
}

BEZEL_TEST(integration, resource_disposal_order) {
    // RAII using-var disposal runs in reverse declaration order.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class R "
        "  public static _log: int := 0; "
        "  public _id: int := 0; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  destructor R._log := R._log * 10 + _id; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  do "
        "    using var a := new R(1); "
        "    using var b := new R(2); "
        "    using var c := new R(3); "
        "  end; "
        "  return R._log; "
        "end Main; end C;"), 65);  // 321 mod 256 = 65 (8-bit exit code)
}

BEZEL_TEST(integration, record_operators_and_pattern_matching) {
    // Record operators + destructuring pattern.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "record Pair(x: int, y: int); "
        "class C "
        "  public static method Sum(p: Pair): int "
        "    return (case p of Pair(a, b) => a + b, _ => 0); "
        "  end Sum; "
        "  public static method Main(): int "
        "    var p := new Pair(10, 20); "
        "    return Sum(p); "
        "  end Main; "
        "end C;"), 30);
}

BEZEL_TEST(integration, binding_pattern_with_guard) {
    // Binding pattern with guard in case expression.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var n := 42; "
        "  return (case n of "
        "    var x when x > 100 => 1, "
        "    var x when x > 10  => 2, "
        "    _ => 3); "
        "end Main; end C;"), 2);
}

BEZEL_TEST(integration, string_operations_chain) {
    // Chained string operations.
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var s := \"  Hello, World!  \"; "
        "  var t := s.Trim().ToLower().Replace(\",\", \"\"); "
        "  return (if t.StartsWith(\"hello\") then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(integration, math_truncate) {
    // Math.Truncate truncates toward zero (not floor).
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  var x := 0.0 - 2.7; "
        "  return int(Math.Truncate(x)); "  // -2, not -3
        "end Main; end C;"), -2 & 255);  // -2 mod 256 = 254
}
