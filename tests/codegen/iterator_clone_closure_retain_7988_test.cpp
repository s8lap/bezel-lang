// A GetEnumerator() clone of a synchronous iterator frame holds its own
// reference to every closure (function-typed) parameter it captured (fix #7988).
//
// §29.3 (v0.359): an iterator method's "arguments are captured into a heap
// state-machine frame that outlives the synchronous call". §29.11 (v0.359):
// "Each `GetEnumerator()` call produces an independent enumerator". §28.9
// (v0.359): function-typed values are runtime-managed, and "Runtime management
// governs the *function value itself* — the heap-allocated closure object that
// backs it". §27.5 (v0.359): "Captured variables are heap-allocated in a
// closure object". So an enumerator handed out by an explicit
// `return GenF(lambda (v: int) => v + s.Length).GetEnumerator();` must keep
// the closure object valid for as long as it can be driven, whatever happens to
// the source enumerable it was cloned from.
//
// Before the fix the frame's construction took one +1 on each closure capture
// (#1864) for the SOURCE frame only; GetEnumerator memcpy'd the frame into the
// clone without a retain, and the clone's Dispose released nothing (it
// "borrowed" from the source). The source temporary of
// `GenF(...).GetEnumerator()` is disposed at the end of that full expression,
// so its Dispose dropped the only reference, the closure (cell + env) was
// reclaimed, and the clone's first MoveNext called freed memory. MEASURED on
// the base (6d709c00f), bezelc --sanitize=address, loop x100: every row below
// except the control is `heap-use-after-free ... in bz$iter$M.Program$...$MoveNext`.
// The non-capturing row shows the freed object is the closure itself.
//
// THE FIX (emitIteratorMethod, LlvmBackend_Iterators.cpp): GetEnumerator takes
// the clone's own bz_closure_retain on each closure capture, and the clone's
// Dispose (cloneDisposeFn) releases it. The async iterator's
// GetAsyncEnumerator already did this (#2609); the string captures got the same
// treatment in #7798.
//
// THE ORACLE. Every row is `runAotChecked` (generated-code ASan + LSan) and
// prints a sum on stdout. The issue's literal repro captures a `string` local;
// with the fix its use-after-free is gone, but it still leaks the string's
// capture CELL (8 B per call) -- the pre-existing #4522 shape 1 / #2622 leak,
// which the same program shows with no clone at all (a plain `for` over
// `GenF(lambda (v: int) => v + s.Length)` leaks it on the base too). It is
// therefore pinned here through its `int`, non-capturing and class twins.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

void expectRun(const std::string& program, const std::string& expected) {
    bezel::test::JitResult r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// The captured `int` twin of the issue's repro: `return GenF(lambda (v: int) => v + b).GetEnumerator();`
// over an `int` local `b`. Base: heap-use-after-free in bz$iter$M.Program$GenF$MoveNext.
BEZEL_TEST(iterator_clone_closure_retain, captured_int_local_clone_returned_spec_29_11_7988) {
    expectRun(R"(
module M;
public class Program
  public static method GenF(f: (int) -> int): IEnumerable<int> for var k in 0..<3 do yield return f(k); end; end GenF;
  public static method En(n: int): IEnumerator<int>
    var b := n * 2;
    return GenF(lambda (v: int) => v + b).GetEnumerator();
  end En;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var e := En(i);
      while e.MoveNext() do t := t + e.Current; end;
    end;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
)", "30000\n");
}

// No capture at all: the freed object is the closure itself, not a capture cell or the
// captured string. Base: heap-use-after-free in bz$iter$M.Program$GenF$MoveNext.
BEZEL_TEST(iterator_clone_closure_retain, non_capturing_lambda_clone_returned_spec_29_11_7988) {
    expectRun(R"(
module M;
public class Box
  public V: int;
  public constructor(v: int) V := v; end constructor;
end Box;
public class Program
  public static method GenF(f: (int) -> int): IEnumerable<int> for var k in 0..<3 do yield return f(k); end; end GenF;
  public static method En(n: int): IEnumerator<int>
    return GenF(lambda (v: int) => v + 1).GetEnumerator();
  end En;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var e := En(i);
      while e.MoveNext() do t := t + e.Current; end;
    end;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
)", "600\n");
}

// The class twin: an instance method's lambda reads the receiver's field (`v + V`), and the
// receiver outlives the enumeration. Base: heap-use-after-free in GenF$MoveNext.
BEZEL_TEST(iterator_clone_closure_retain, class_this_capture_clone_returned_spec_29_11_7988) {
    expectRun(R"(
module M;
public class Box
  public V: int;
  public constructor(v: int) V := v; end constructor;
  public method En(): IEnumerator<int>
    return Program.GenF(lambda (v: int) => v + V).GetEnumerator();
  end En;
end Box;
public class Program
  public static method GenF(f: (int) -> int): IEnumerable<int> for var k in 0..<3 do yield return f(k); end; end GenF;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      var b := new Box(i);
      using var e := b.En();
      while e.MoveNext() do t := t + e.Current; end;
    end;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
)", "15150\n");
}

// Two function-typed parameters around a non-function one: each closure capture gets its own
// clone reference. Base: heap-use-after-free in Gen2$MoveNext.
BEZEL_TEST(iterator_clone_closure_retain, two_closure_params_clone_returned_spec_29_11_7988) {
    expectRun(R"(
module M;
public class Program
  public static method Gen2(f: (int) -> int, n: int, g: (int) -> int): IEnumerable<int> for var k in 0..<n do yield return f(k) + g(k); end; end Gen2;
  public static method En(n: int): IEnumerator<int>
    var a := n; var b := n * 2;
    return Gen2(lambda (v: int) => v + a, 3, lambda (v: int) => v * b).GetEnumerator();
  end En;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var e := En(i);
      while e.MoveNext() do t := t + e.Current; end;
    end;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
)", "44850\n");
}

// A generic iterator `GenT<T>(x: T, f: (int) -> int)` instantiated at `int`.
// Base: heap-use-after-free in GenT$MoveNext.
BEZEL_TEST(iterator_clone_closure_retain, generic_iterator_closure_param_clone_returned_spec_29_11_7988) {
    expectRun(R"(
module M;
public class Program
  public static method GenT<T>(x: T, f: (int) -> int): IEnumerable<int> for var k in 0..<3 do yield return f(k); end; end GenT;
  public static method En(n: int): IEnumerator<int>
    var a := n;
    return GenT<int>(n, lambda (v: int) => v + a).GetEnumerator();
  end En;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do
      using var e := En(i);
      while e.MoveNext() do t := t + e.Current; end;
    end;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
)", "15150\n");
}

// BOUNDARY CONTROL (green on the base): one source enumerated by two `for`
// loops and two explicit GetEnumerator() clones, 100 times. Every clone's
// retain must be matched by its own Dispose's release. A fix that retained in
// GetEnumerator but left the clone's Dispose at releaseClosures=false leaks
// one closure per clone here (LSan); a fix that released in the clone without
// the retain is a use-after-free.
BEZEL_TEST(iterator_clone_closure_retain, control_for_loops_and_clones_balanced_spec_29_11_7988) {
    expectRun(R"(
module M;
public class Program
  public static method GenF(f: (int) -> int): IEnumerable<int> for var k in 0..<3 do yield return f(k); end; end GenF;
  public static method Sum(n: int): int
    var t := 0;
    var a := n;
    var src := GenF(lambda (v: int) => v + a);
    for var x in src do t := t + x; end;
    using var e1 := src.GetEnumerator();
    using var e2 := src.GetEnumerator();
    while e1.MoveNext() do t := t + e1.Current; end;
    while e2.MoveNext() do t := t + e2.Current; end;
    for var x in src do t := t + x; end;
    return t;
  end Sum;
  public static method Main(): int
    var t := 0;
    for var i in 0..<100 do t := t + Sum(i); end;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
)", "60600\n");
}
