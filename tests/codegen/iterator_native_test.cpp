// Codegen tests for IEnumerable iteration and iterator methods (§29.2, §29.3).
//
// These tests use the native compile+run harness (same as async_test.cpp) rather
// than the in-process JIT.  The iterator/async emitter accumulates LLVM module
// state across JIT runs (multiple modules in one process), so the JIT harness
// produces unreliable results for iterator methods; see the comment at the top
// of async_test.cpp.  The original iterator_test.cpp is kept out of the CMake
// suite for the same reason this file was created — all new iterator codegen
// tests go here.
//
// #437: This file covers the two headline bugs from #437 plus the full §29 test
// matrix:
//   1. yield break inside a loop → well-formed IR (was: missing terminator).
//   2. iterator passed as IEnumerable<T> param → vtable dispatch (was: LLVM fatal).
//
// All tests compile the program to a native executable via Driver::compileFile
// and check the process exit code.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a native executable and run it; returns the exit code,
// or -1 on compile/link/run failure.  Each call uses a unique temp-file stem so
// concurrent invocations (if any) don't collide.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_iter_" + std::to_string(counter++);
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
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

}  // namespace

// ---------------------------------------------------------------------------
// §29.2 Part A: user-defined IEnumerable<T> for-loop (class implementing the
// interface explicitly via GetEnumerator / MoveNext / Current).
// ---------------------------------------------------------------------------

static const char* kIntRangeBase = R"(
  class IntRangeEnum: IEnumerator<int>
    private lo: int; private hi: int; private cur: int;
    public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; cur:=lo-1; end constructor;
    public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext;
    public readonly property Current: int get => cur; end Current;
    public method Dispose() end Dispose;
  end IntRangeEnum;
  class IntRange: IEnumerable<int>
    private lo: int; private hi: int;
    public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; end constructor;
    public method GetEnumerator(): IEnumerator<int> return new IntRangeEnum(lo, hi); end GetEnumerator;
  end IntRange;
)";

// §29.2-A: basic for-over-IEnumerable; sum 1..5 = 15.
BEZEL_TEST(iter_native_a, user_ienumerable_sum) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M;") + kIntRangeBase +
        "class C public static method Main(): int "
        "  using var r := new IntRange(1, 5); var s := 0; "
        "  for var x in r do s := s + x; end; return s; "
        "end Main; end C;"), 15);
}

// §29.2-A: for-over-IEnumerable with early break; sum stops at x>=5.
BEZEL_TEST(iter_native_a, user_ienumerable_break) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M;") + kIntRangeBase +
        "class C public static method Main(): int "
        "  using var r := new IntRange(1, 100); var s := 0; "
        "  for var x in r do s:=s+x; if x>=5 then break; end; end; return s; "
        "end Main; end C;"), 15);
}

// ---------------------------------------------------------------------------
// §29.2 Part B1: Array<T> as IEnumerable<T> parameter.
// ---------------------------------------------------------------------------

// §29.2-B1: int array passed to Sum(IEnumerable<int>).
BEZEL_TEST(iter_native_b1, array_as_ienumerable_param) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static method Sum(items: IEnumerable<int>): int "
        "    var s := 0; for var x in items do s:=s+x; end; return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "    return Sum(a); "
        "  end Main; end C;"), 15);
}

// §29.2-B1: string array passed to Count(IEnumerable<string>).
BEZEL_TEST(iter_native_b1, string_array_as_ienumerable) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static method Count(items: IEnumerable<string>): int "
        "    var n := 0; for var x in items do n:=n+1; end; return n; "
        "  end Count; "
        "  public static method Main(): int "
        "    using var a := new Array<string>(); "
        "    a.Add(\"hi\"); a.Add(\"hello\"); a.Add(\"world\"); "
        "    return Count(a); "
        "  end Main; end C;"), 3);
}

// ---------------------------------------------------------------------------
// §29.3 Part C: yield return / iterator methods (state-machine codegen).
// ---------------------------------------------------------------------------

// §29.3: simple straight-line yield sequence; 1+2+3 = 6.
BEZEL_TEST(iter_native_c, simple_sequence) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static method OneTwoThree(): IEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end OneTwoThree; "
        "  public static method Main(): int "
        "    var s := 0; for var x in OneTwoThree() do s:=s+x; end; return s; "
        "  end Main; end C;"), 6);
}

// §29.3: yield inside a range-for loop; squares 1..4 = 1+4+9+16 = 30.
BEZEL_TEST(iter_native_c, range_for_yield) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static method Squares(n: int): IEnumerable<int> "
        "    for var i in 1..n do yield return i*i; end; "
        "  end Squares; "
        "  public static method Main(): int "
        "    var s := 0; for var x in Squares(4) do s:=s+x; end; return s; "
        "  end Main; end C;"), 30);
}

// §29.3: yield inside a while loop with a conditional; odd numbers up to 10.
BEZEL_TEST(iter_native_c, while_yield_with_condition) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static method Odds(max: int): IEnumerable<int> "
        "    var i := 1; while i <= max do "
        "      if i mod 2 = 1 then yield return i; end; "
        "      i := i + 1; end; "
        "  end Odds; "
        "  public static method Main(): int "
        "    var s := 0; for var x in Odds(10) do s:=s+x; end; return s; "
        "  end Main; end C;"), 25);  // 1+3+5+7+9=25
}

// §29.3: Fibonacci iterator (loop-carried multi-local state).
// 8 elements of sequence starting at 0: 0,1,1,2,3,5,8,13. Last = 13.
BEZEL_TEST(iter_native_c, fibonacci_iterator) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static method Fibonacci(n: int): IEnumerable<int> "
        "    var a := 0; var b := 1; var count := 0; "
        "    while count < n do "
        "      yield return a; "
        "      var t := a + b; a := b; b := t; "
        "      count := count + 1; "
        "    end; "
        "  end Fibonacci; "
        "  public static method Main(): int "
        "    var last := 0; "
        "    for var x in Fibonacci(8) do last := x; end; "
        "    return last; "
        "  end Main; end C;"), 13);
}

// ---------------------------------------------------------------------------
// §29.3 yield break tests (#437 headline fix).
//
// Before #437, a `yield break` inside a for/while loop caused the MoveNext
// state-machine emitter to create an extra state block (one for the yield break
// itself, even though yield break does NOT consume a resume-state slot) that
// was left without a terminator.  The IR verifier caught this as a hard error.
// These tests verify that:
//   (a) the program compiles without IR-verifier errors, and
//   (b) it produces the correct prefix of values.
// ---------------------------------------------------------------------------

// §29.3 (#437-A): yield break inside a range-for loop.
// Limited(5) yields 1..5 then breaks; sum = 15.
BEZEL_TEST(iter_native_c_437, yield_break_in_for_loop) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static method Limited(n: int): IEnumerable<int> "
        "    for var i in 1..100 do "
        "      if i > n then yield break; end; "
        "      yield return i; "
        "    end; "
        "  end Limited; "
        "  public static method Main(): int "
        "    var s := 0; for var x in Limited(5) do s:=s+x; end; return s; "
        "  end Main; end C;"), 15);
}

// §29.3 (#437-B): yield break inside a while loop.
// WhileLimited(4) yields 1..4 then breaks; sum = 10.
BEZEL_TEST(iter_native_c_437, yield_break_in_while_loop) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static method WhileLimited(n: int): IEnumerable<int> "
        "    var i := 1; "
        "    while true do "
        "      if i > n then yield break; end; "
        "      yield return i; "
        "      i := i + 1; "
        "    end; "
        "  end WhileLimited; "
        "  public static method Main(): int "
        "    var s := 0; for var x in WhileLimited(4) do s:=s+x; end; return s; "
        "  end Main; end C;"), 10);
}

// §29.3 (#437-C): unconditional yield break (no prior yield return).
// The iterator is immediately exhausted; sum = 0.
BEZEL_TEST(iter_native_c_437, yield_break_only) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static method Empty(): IEnumerable<int> "
        "    yield break; "
        "  end Empty; "
        "  public static method Main(): int "
        "    var s := 0; for var x in Empty() do s:=s+x; end; return s; "
        "  end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// §29.3 (#437-D): iterator passed as IEnumerable<T> parameter.
//
// Before #437, a parameter typed as IEnumerable<T> (a builtin interface) was
// misidentified as array-backed (bz_array_t* protocol) because the vtable-path
// heuristic required a user-class declaration pointer.  Passing an iterator
// state machine through an IEnumerable<T> parameter caused an LLVM fatal.
// ---------------------------------------------------------------------------

// §29.3 (#437-D): Range(1,10) iterator passed to Sum(IEnumerable<int>); sum = 55.
BEZEL_TEST(iter_native_c_437, iterator_as_ienumerable_param) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static method Range(lo: int, hi: int): IEnumerable<int> "
        "    var i := lo; while i <= hi do yield return i; i:=i+1; end; "
        "  end Range; "
        "  public static method Sum(items: IEnumerable<int>): int "
        "    var s := 0; for var x in items do s:=s+x; end; return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    return Sum(Range(1, 10)); "
        "  end Main; end C;"), 55);
}
