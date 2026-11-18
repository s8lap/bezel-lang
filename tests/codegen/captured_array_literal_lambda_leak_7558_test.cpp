// Codegen — issue #7558: a lambda that captures a borrowed `Array<int>` local
// (directly, or through a same-collection / `IEnumerable<int>` view alias)
// leaked the array once per call of the enclosing method: 160 B in 2
// allocations (the `bz_array_t` header and its data block).
//
// THE RULE. §28.9 (v0.359, "Runtime-managed types" / trivial-dispose): an
// `Array<T>` is trivial-dispose, but its backing storage is reclaimed with the
// instance, here at the owner's scope end. §27.5 (v0.359, "Closures"): a
// lambda captures a variable by reference into a heap closure; a closure that
// does not outlive the capturing scope (called and dropped locally) holds no
// claim on the variable past that scope's end.
//
// ROOT CAUSE. The leaked block is the ARRAY, not the closure env or its
// capture cell. `var a: Array<int> := [1,2,3]` is reclaimed at scope end only
// if collectBorrowingArrayFreeEligible's escape walk proves `a` (and every
// view alias of it) never escapes, and that walk counted ANY read of the local
// inside a lambda body as an escape ("the env outlives the scope"). A lambda
// that is proven non-escaping (`cellFreeEligible_`) was thereby treated like
// one stored in a field, and the array was never freed. The fix lifts that
// override for a SAFE read (receiver of a member/`?.` access, comparison
// operand) of a local whose every capturing lambda is non-escaping; the cell
// is then freed after the array's disposal (the #1017 second pass).
//
// Each program calls the leaking method 100 times (200 in the rows added by
// the second fix-forward), so a leak is 100 allocations
// and cannot hide behind a stale stack slot (a single leak of this shape is
// reported by LeakSanitizer only intermittently). Rows run under
// `runAotChecked` (AddressSanitizer + LeakSanitizer) and compare stdout.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// stdout of a checked run (ASan + LSan) that compiled, ran and exited 0, or a
// marker naming what failed.
std::string checkedOut(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// The issue's repro: an `IEnumerable<int>?` view of the array captured by a
// lambda that calls two Linq terminals through `?.`. 100 * (3+6 twice) = 1800.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           issue_repro_view_alias_linq_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M; import System; import System.Linq;
class P
  static method One(): int
    var a: Array<int> := [1,2,3];
    var c: IEnumerable<int>? := a;
    var f := lambda() => (c?.Count() ?? -1) + (c?.Sum() ?? 0);
    return f() + f();
  end One;
  public static method Main(): int
    var s := 0;
    for var i in 1..100 do s := s + One(); end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "1800\n");
}

// The `?.` member form through a nullable same-collection alias.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           nullable_alias_member_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M; import System;
class P
  static method One(): int
    var a: Array<int> := [1,2,3];
    var c: Array<int>? := a;
    var f := lambda() => (c?.Count ?? -1);
    return f() + f();
  end One;
  public static method Main(): int
    var s := 0;
    for var i in 1..100 do s := s + One(); end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "600\n");
}

// The array itself captured, no alias.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           direct_capture_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M; import System;
class P
  static method One(): int
    var a: Array<int> := [1,2,3];
    var f := lambda() => (a.Count);
    return f();
  end One;
  public static method Main(): int
    var s := 0;
    for var i in 1..100 do s := s + One(); end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "300\n");
}

// ---------------------------------------------------------------------------
// fix-forward (review r42-rev-s55): a lambda read whose RESULT can carry the
// array's storage out of the lambda keeps the array alive.
//
// THE RULE. §28.9 (v0.359): runtime management of a closure "relaxes nothing
// about the values the closure captures: a captured borrowed class reference
// is subject to §28.3's borrow-escape rules". `a.GetEnumerator()` and
// `a.AsReadOnlySpan()` are borrows of `a` (§28.3 borrow-lifetime rule: outside
// a lambda, `return a.GetEnumerator();` from this method is rejected), and
// through `return f();` such a borrow reaches the caller. The first #7558 fix
// lifted the lambda-read escape at EVERY safe-read position, so these rows
// freed the array at `Make`'s scope end and the caller read it
// (heap-use-after-free in `bz$arrenum$...$Current` / the span read). The
// fix-forward lifts it only for a read whose chain reaches a value copy.
//
// These rows pin MEMORY SAFETY and the output: they run under
// `runAotCheckedLenient` (generated-code ASan, LeakSanitizer off), because the
// sound codegen verdict for a borrow that escapes through the lambda's result
// is the conservative one, which keeps (leaks) the array. Red on the first
// fix (b9ebb9728): heap-use-after-free, ran=false.
// ---------------------------------------------------------------------------

namespace {

std::string lenientOut(const std::string& program) {
    auto r = runAotCheckedLenient(program);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// rev-s55 p27: an enumerator over the captured array, returned through `f()`.
// 100 * (1+2+3) = 600.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           lambda_returns_enumerator_no_uaf_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method Make(): IEnumerator<int>
    var a: Array<int> := [1,2,3];
    var f := lambda() => a.GetEnumerator();
    return f();
  end Make;
  public static method Main(): int
    var s := 0;
    for var i in 1..100 do var e := Make(); while e.MoveNext() do s := s + e.Current; end; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "600\n");
}

// rev-s55 p42: a read-only span over the captured array, returned through
// `f()`. 100 * (1+3) = 400.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           lambda_returns_span_no_uaf_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method Make(): ReadOnlyArraySpan<int>
    var a: Array<int> := [1,2,3];
    var f := lambda() => a.AsReadOnlySpan();
    return f();
  end Make;
  public static method Main(): int
    var s := 0;
    for var i in 1..100 do var sp := Make(); s := s + sp[0] + sp[2]; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "400\n");
}


// ---------------------------------------------------------------------------
// second fix-forward (review r42-rev-s66): a `string` step is not a copy.
//
// THE RULE. §4.6 / §28.9 (v0.359): `string` is a runtime-managed reference
// type, so reading an `Array<string>` element yields the string the array
// holds, not a copy of it; §28.9: closure runtime management "relaxes nothing
// about the values the closure captures". The first fix-forward counted a
// `string` step as a value copy at ANY position of the read's chain, so a
// later step that borrows the string (`.AsSpan(..)`) still lifted the array's
// escape verdict: the scope-end array free released the dynamic element
// string and the caller's span read crashed (SEGV in `bz_str_slice_cp`). That
// a string span keeps no source alive is #8191, not fixed here.
//
// Each row returns, through `return f();`, a span over a DYNAMIC element
// string (`$"q{n}zzz"`), and the caller reads it 200 times. Rows pin MEMORY
// SAFETY and the output under `runAotCheckedLenient` (ASan on, LSan off): the
// sound codegen verdict here is the conservative one, which keeps (leaks) the
// array. Red on the first fix-forward (b39d36f6d): SEGV, ran=false.
// ---------------------------------------------------------------------------

// rev-s66 r02: `lambda() => a[1].AsSpan()`. Expected stdout 1292.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           lambda_returns_string_element_span_no_crash_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method Make(n: int): ReadOnlyStringSpan
    var a: Array<string> := ["ab", $"q{n}zzz", "c"];
    var f := lambda() => a[1].AsSpan();
    return f();
  end Make;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do var sp := Make(i); s := s + sp.ToString().Length; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "1292\n");
}

// rev-s66 r06: `c!.Last().AsSpan()` through an `Array<string>?` view alias. Expected stdout 1292.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           lambda_returns_view_alias_last_span_no_crash_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method Make(n: int): ReadOnlyStringSpan
    var a: Array<string> := ["ab", "c", $"q{n}zzz"];
    var c: Array<string>? := a;
    var f := lambda() => c!.Last().AsSpan();
    return f();
  end Make;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do var sp := Make(i); s := s + sp.ToString().Length; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "1292\n");
}

// rev-s66 r07: `Id(a[1]).AsSpan()` through a generic identity. Expected stdout 1292.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           lambda_returns_generic_id_element_span_no_crash_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method Id<T>(x: T): T => x;
  static method Make(n: int): ReadOnlyStringSpan
    var a: Array<string> := ["ab", $"q{n}zzz", "c"];
    var f := lambda() => Id(a[1]).AsSpan();
    return f();
  end Make;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do var sp := Make(i); s := s + sp.ToString().Length; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "1292\n");
}

// rev-s66 r07b: `a.ElementAt(1).AsSpan()`. Expected stdout 1292.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           lambda_returns_elementat_span_no_crash_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method Make(n: int): ReadOnlyStringSpan
    var a: Array<string> := ["ab", $"q{n}zzz", "c"];
    var f := lambda() => a.ElementAt(1).AsSpan();
    return f();
  end Make;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do var sp := Make(i); s := s + sp.ToString().Length; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "1292\n");
}

// rev-s66 r10b: a `for` over the `Array<string>` returning `x.AsSpan()` (the for-in arm). Expected stdout 1292.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           lambda_forin_string_element_span_no_crash_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method Make(n: int): ReadOnlyStringSpan
    var a: Array<string> := [$"q{n}zzz", "c"];
    var f := lambda(): ReadOnlyStringSpan => do for var x in a do return x.AsSpan(); end; return "".AsSpan(); end;
    return f();
  end Make;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do var sp := Make(i); s := s + sp.ToString().Length; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "1292\n");
}

// rev-s66 r15: `var s := a[1]; return s.AsSpan();` (the string reaches the span through a block local). Expected stdout 1292.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           lambda_block_local_string_element_span_no_crash_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method Make(n: int): ReadOnlyStringSpan
    var a: Array<string> := ["ab", $"q{n}zzz", "c"];
    var f := lambda(): ReadOnlyStringSpan => do var s := a[1]; return s.AsSpan(); end;
    return f();
  end Make;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do var sp := Make(i); s := s + sp.ToString().Length; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "1292\n");
}

// rev-s66 r16: `a[1].AsSpan(1, 2)`. Expected stdout 400.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           lambda_returns_string_element_subspan_no_crash_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method Make(n: int): ReadOnlyStringSpan
    var a: Array<string> := ["ab", $"q{n}zzz", "c"];
    var f := lambda() => a[1].AsSpan(1, 2);
    return f();
  end Make;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do var sp := Make(i); s := s + sp.ToString().Length; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "400\n");
}

// ---------------------------------------------------------------------------
// #7851: `lambda(x: int) => a.IndexOf(x)` over a captured `Array<int>` leaked
// the array once per call of the enclosing method (32000 B in 400
// allocations over 200 calls on r30-c1). The read's chain reaches an `int`
// step, a value copy, so the array is reclaimed at scope end (§28.9, §27.5).
// `runAotChecked` (ASan + LSan), 200 calls.
// ---------------------------------------------------------------------------

// `a.IndexOf(x)`: 200 * (0 + 1) = 200.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           issue_7851_indexof_reclaimed_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M; import System; import System.Linq;
class P
  static method One(): int
    var a: Array<int> := [1,2,1]; var f := lambda(x: int) => a.IndexOf(x);
    return f(1) + f(2);
  end One;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do s := s + One(); end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "200\n");
}

// `a.LastIndexOf(x)`: 200 * (2 + 1) = 600.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           issue_7851_lastindexof_reclaimed_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M; import System; import System.Linq;
class P
  static method One(): int
    var a: Array<int> := [1,2,1]; var f := lambda(x: int) => a.LastIndexOf(x);
    return f(1) + f(2);
  end One;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do s := s + One(); end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "600\n");
}
