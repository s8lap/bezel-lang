// Spec-derived regression test for issue #7863 (and its siblings found while
// measuring it) — a `byte` / `sbyte` / `short` / `ushort` / `uint` argument at
// an integer parameter of a built-in constructor other than the
// DateTime / DateOnly / TimeOnly family fixed in #7784 (campaign
// builtin-call-admission, lane C2-48).
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ─────────────────────
//
// stdlib §6.3 declares `Semaphore(initialCount: int)` and
//   `Semaphore(initialCount: int, maxCount: int)`; §6.9 `Channel<T>(capacity:
//   int)`; §4.4 `Array<T>(size: int, fill: T)`; §4.14 `OwningBuffer<T>(length:
//   int)` and the `Span` / `ReadOnlySpan` `(source, start: int, length: int)`
//   windows; §2.7 `Random(seed: long)`; §2.15 `JsonOptions(..., IndentWidth:
//   int, ..., MaxDepth: int)`.
// spec §5.1: an implicit widening conversion takes a narrower integer type to
//   a wider one (`byte`/`sbyte`/`short`/`ushort` to `int`, and those and `uint`
//   to `long`), preserving the value; the callee reads the VALUE the argument
//   denotes (`byte` 200 is 200, `uint` 4000000000 is 4000000000).
//
// ─── THE DEFECT (MEASURED on staging 26216e9f) ──────────────────────────────
//
// `new Semaphore(b)` with `b: byte` (or `short`) aborted the compiler in the
// LLVM verifier ("DestTy too big for Trunc: trunc i8 to i32"), as did the
// Span / ReadOnlySpan windows.  `new Array<int>(b, 7)`, `new Channel<int>(b)`,
// `new OwningBuffer<int>(b)` and `new Random(u)` SIGN-extended an unsigned
// source: byte 200 read as -56 (a spurious ArgumentOutOfRangeException /
// ArgumentException) and uint 4000000000 as a negative seed.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; import System.Threading; "
           "import System.Serialization; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// The issue's repro: a `byte` one-argument Semaphore.
BEZEL_TEST(builtin_ctor_narrow_int_args_7863, semaphore_byte_one_arg) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var b: byte := 3; using var s := new Semaphore(b); "
        "    using var t1 := s.Acquire(); using var t2 := s.Acquire(); "
        "    using var t3 := s.Acquire(); Console.WriteLine(\"acquired\");")),
        "acquired");
}

// The sibling #7863 comment: a `short`.
BEZEL_TEST(builtin_ctor_narrow_int_args_7863, semaphore_short_one_arg) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var b: short := 2; using var s := new Semaphore(b); "
        "    using var t1 := s.Acquire(); using var t2 := s.Acquire(); "
        "    Console.WriteLine(\"acquired\");")),
        "acquired");
}

// ZERO-extension at both Semaphore parameters: initial byte 100 <= max byte 200
// builds (a sign-extended max of -56 would raise ArgumentException); initial
// byte 200 > max byte 100 raises (a sign-extended initial of -56 would not).
BEZEL_TEST(builtin_ctor_narrow_int_args_7863, semaphore_two_arg_zero_extends) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var lo: byte := 100; var hi: byte := 200; var us: ushort := 60000; "
        "    var built := 0; var raised := 0; "
        "    try using var s := new Semaphore(lo, hi); built := 1; "
        "    catch ArgumentException => do built := -1; end end; "
        "    try using var s2 := new Semaphore(lo, us); built := built + 1; "
        "    catch ArgumentException => do built := -10; end end; "
        "    try using var s3 := new Semaphore(hi, lo); "
        "    catch ArgumentException => do raised := 1; end end; "
        "    Console.WriteLine(built); Console.WriteLine(raised);")),
        "2\n1");
}

// Named arguments (§22.5), reordered, over narrow types.
BEZEL_TEST(builtin_ctor_narrow_int_args_7863, semaphore_named_narrow) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var m: byte := 200; var i: sbyte := 5; "
        "    using var s := new Semaphore(maxCount := m, initialCount := i); "
        "    using var t1 := s.Acquire(); Console.WriteLine(\"ok\");")),
        "ok");
}

// `Array<T>(size, fill)`: byte 200 and ushort 60000 are sizes, not negatives.
BEZEL_TEST(builtin_ctor_narrow_int_args_7863, array_size_fill_zero_extends) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var b: byte := 200; var a := new Array<int>(b, 7); "
        "    Console.WriteLine(a.Count); "
        "    var u: ushort := 60000; var a2 := new Array<int>(u, 1); "
        "    Console.WriteLine(a2.Count);")),
        "200\n60000");
}

// `Array<T>(capacity)` with a byte above the signed midpoint must not fault.
BEZEL_TEST(builtin_ctor_narrow_int_args_7863, array_capacity_byte_above_127) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var b: byte := 200; var a := new Array<int>(b); "
        "    a.Add(5); Console.WriteLine(a.Count);")),
        "1");
}

// `Channel<T>(capacity)`: byte 200 is a valid capacity (>= 1), not -56.
BEZEL_TEST(builtin_ctor_narrow_int_args_7863, channel_capacity_byte_above_127) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var b: byte := 200; var ok := 0; "
        "    try using var ch := new Channel<int>(b); ok := 1; "
        "    catch ArgumentException => do ok := -1; end end; "
        "    var s: short := 4; "
        "    try using var ch2 := new Channel<int>(s); ok := ok + 1; "
        "    catch ArgumentException => do ok := -10; end end; "
        "    Console.WriteLine(ok);")),
        "2");
}

// `OwningBuffer<T>(length)`: byte 200 is length 200.
BEZEL_TEST(builtin_ctor_narrow_int_args_7863, owning_buffer_length_zero_extends) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var b: byte := 200; using var buf := new OwningBuffer<int>(b); "
        "    Console.WriteLine(buf.Length);")),
        "200");
}

// `Random(seed: long)`: a `uint` seed zero-extends, a `byte` seed is its value.
BEZEL_TEST(builtin_ctor_narrow_int_args_7863, random_seed_unsigned_zero_extends) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var u: uint := 4000000000; var r1 := new Random(u); "
        "    var r2 := new Random(4000000000L); "
        "    Console.WriteLine(r1.Next() = r2.Next()); "
        "    var b: byte := 200; var r3 := new Random(b); "
        "    var r4 := new Random(200L); "
        "    Console.WriteLine(r3.Next() = r4.Next()); "
        "    var w: ushort := 60000; var r5 := new Random(w); "
        "    var r6 := new Random(60000L); "
        "    Console.WriteLine(r5.Next() = r6.Next());")),
        "True\nTrue\nTrue");
}

// Span / ReadOnlySpan windows: byte `start` 200 and `length` 5 over a 300-slot
// array read elements 200..204 (aborted the compiler on the base).
BEZEL_TEST(builtin_ctor_narrow_int_args_7863, span_window_narrow_start_length) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var a := new Array<int>(300, 0); var i := 0; "
        "    while i < 300 do a[i] := i; i := i + 1; end; "
        "    var s: byte := 200; var l: ushort := 5; "
        "    var sp := new Span<int>(a, s, l); "
        "    Console.WriteLine(sp.Length); Console.WriteLine(sp[0]); "
        "    var rs := new ReadOnlySpan<int>(a, s, l); "
        "    Console.WriteLine(rs.Length); Console.WriteLine(rs[4]);")),
        "5\n200\n5\n204");
}

// `JsonOptions(Indented, IndentWidth, ..., MaxDepth)` with narrow ints: a `byte`
// IndentWidth of 130 indents the one field by 130 spaces (the output is longer
// than 130), and a `short` MaxDepth of 99 admits the one-level record.
BEZEL_TEST(builtin_ctor_narrow_int_args_7863, json_options_narrow_ints) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; import System.Serialization; "
        "record R(X: int): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var w: byte := 130; var d: short := 99; "
        "  var opts := new JsonOptions(true, w, false, false, d); "
        "  using var s := new JsonSerializer(opts); "
        "  var r := new R(1); r.Serialize(s); "
        "  var json := s.ToString(); "
        "  Console.WriteLine(json.Length > 130); "
        "  return 0; "
        "end Main; end C;"),
        "True");
}

// Control: `int` arguments are unchanged.
BEZEL_TEST(builtin_ctor_narrow_int_args_7863, int_args_control) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var n: int := 9; using var s := new Semaphore(n, 20); "
        "    using var t := s.Acquire(); "
        "    var a := new Array<int>(n, 1); Console.WriteLine(a.Count);")),
        "9");
}
