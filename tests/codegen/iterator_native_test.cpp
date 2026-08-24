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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirIter() {
    return wtScratchDir("wt-iter");
}

// Compile `source` to a native executable and run it; returns the exit code,
// or -1 on compile/link/run failure.  Each call uses a unique temp-file stem so
// concurrent invocations (if any) don't collide.
int compileAndRun(const std::string& source) {
    auto dir = wtDirIter();
    std::string stem = unique_stem("bezel_iter");
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
    public property Current: int get => cur; end Current;
    // §28.2: Dispose() is compiler-synthesised; no user declaration needed.
    // IEnumerator<T>'s IDisposable requirement is satisfied by the compiler-
    // generated Dispose() that every class implicitly carries.
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

// fix #747 R43 / §29.2: an Array<T> assigned to an IEnumerable<T> LOCAL must be
// wrapped in a vtable-backed enumerator, exactly as an Array<T> argument passed
// to an IEnumerable<T> PARAMETER is (iter_native_b1 above). Previously only the
// parameter and the user-class-local cases were wrapped, so iterating an
// `IEnumerable<T>` local backed by an array dereferenced the bz_array_t layout
// as a vtable → SIGSEGV. The richer interfaces (IList/IReadOnlyList) keep the
// array-backed protocol and are not wrapped.
BEZEL_TEST(iter_native_b1, array_in_ienumerable_local) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); "
        "  var seq: IEnumerable<int> := a; "
        "  var sum := 0; for var x in seq do sum := sum + x; end; "
        "  return sum; "  // 30
        "end Main; end C;"), 30);
}

// String element type through an IEnumerable<string> local.
BEZEL_TEST(iter_native_b1, string_array_in_ienumerable_local) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"ab\"); a.Add(\"cde\"); "
        "  var seq: IEnumerable<string> := a; "
        "  var total := 0; for var s in seq do total := total + s.Length; end; "
        "  return total; "  // 2 + 3 = 5
        "end Main; end C;"), 5);
}

// An empty array through an IEnumerable<int> local iterates zero times.
BEZEL_TEST(iter_native_b1, empty_array_in_ienumerable_local) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  var seq: IEnumerable<int> := a; "
        "  var sum := 0; for var x in seq do sum := sum + x; end; "
        "  return sum + 7; "  // 7
        "end Main; end C;"), 7);
}

// CONTROL §29.2: an IList<int> local keeps the array-backed protocol (it is
// NOT wrapped) and still iterates correctly — the fix must not change it.
BEZEL_TEST(iter_native_b1, ilist_local_unwrapped_control) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); "
        "  var seq: IList<int> := a; "
        "  var sum := 0; for var x in seq do sum := sum + x; end; "
        "  return sum; "  // 30
        "end Main; end C;"), 30);
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

// ---------------------------------------------------------------------------
// §29.2 / §29.3 (#552): user-defined IEnumerable<T> iterated via interface type.
//
// Before #552, `for x in items` where `items` was STATICALLY typed as
// IEnumerable<T> but held a user-class instance (not an iterator state machine
// or wrapped Array<T>) silently miscompiled: the codegen treated the class
// object as if it were already an enumerator (vtable slots [2]=MoveNext,
// [3]=Current), but a class object has [2]=Dispose / [3]=ToString → wrong
// result (0 instead of 15).  Fix: wrap the user-class argument in a thin
// sm-compatible enumerator wrapper at the call site (buildOrderedArgs) and at
// local-variable initialisation sites.
// ---------------------------------------------------------------------------

// §29.2 (#552-A): user class passed to IEnumerable<T> param; sum 1..5 = 15.
BEZEL_TEST(iter_native_552, user_class_as_ienumerable_param) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M;") + kIntRangeBase +
        "class C "
        "  public static method Sum(items: IEnumerable<int>): int "
        "    var s := 0; for var x in items do s:=s+x; end; return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var r := new IntRange(1, 5); "
        "    return Sum(r); "
        "  end Main; end C;"), 15);
}

// §29.2 (#552-B): user class stored in IEnumerable<int> local then iterated.
// Uses `using var` so the underlying object is properly disposed.
BEZEL_TEST(iter_native_552, user_class_in_ienumerable_local) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M;") + kIntRangeBase +
        "class C "
        "  public static method Main(): int "
        "    using var items: IEnumerable<int> := new IntRange(1, 5); "
        "    var s := 0; for var x in items do s:=s+x; end; return s; "
        "  end Main; end C;"), 15);
}

// §29.2 (#552-C): user class iterated twice through IEnumerable<T> param
// (each call to Sum allocates a fresh enumerator via GetEnumerator).
BEZEL_TEST(iter_native_552, user_class_ienumerable_double_sum) {
    BEZEL_EXPECT_EQ(compileAndRun(
        std::string("module M;") + kIntRangeBase +
        "class C "
        "  public static method Sum(items: IEnumerable<int>): int "
        "    var s := 0; for var x in items do s:=s+x; end; return s; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var r := new IntRange(1, 5); "
        "    var a := Sum(r); "
        "    var b := Sum(r); "
        "    return a + b; "
        "  end Main; end C;"), 30);
}
