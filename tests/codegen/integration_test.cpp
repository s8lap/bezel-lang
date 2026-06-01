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
#include "test_harness.hpp"

namespace {

int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_integ_" + std::to_string(counter++);
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
        "module M; class AppError end AppError; class C "
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
        "  var r := case x 1 => 1, 2 => 22, _ => 0 end; "
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
        "  var c := a with x := 9 end; "
        "  return (1 if a = b else 0) + (1 if a <> c else 0); "
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
        "    return case s.value "
        "      90..100 => 4, 80..89 => 3, 70..79 => 2, "
        "      60..69 => 1, _ => 0 end; "
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
