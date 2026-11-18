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
// SUITE SHAPE (owner's base-green cap, 2026-11-12). This suite holds only:
//   - FIVE RED rows: the three fix rows and the two #7851 rows. Each is red
//     on r30-c1 (the leak oracle) and green on the branch tip. None of them
//     is touched by this reshape.
//   - TWO base-green boundary controls, each a MULTI-SPELLING program: the
//     spellings are looped inside ONE program and the whole stdout is
//     compared. They are the suite's only base-green rows (the cap), and each
//     is red under a plausible wrong fix named in the commit body.
// Every other shape this suite has ever held is a corpus candidate under
// /var/tmp/bezel-scratch/r42-m99-7558/corpus-candidates/ (a program that is
// green with and without the fix, but whose verdict a spec sentence fixes,
// belongs in the conformance corpus, not in gtest).
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
// BOUNDARY CONTROL 1 of 2 (owner cap 2026-11-12): a read whose result can
// carry the array's storage out of the lambda keeps the array alive.
//
// THE RULE. §28.9 (v0.359): runtime management of a closure "relaxes nothing
// about the values the closure captures: a captured borrowed class reference
// is subject to §28.3's borrow-escape rules". §28.3.8 (v0.359, "Borrow
// lifetime rules"): "A borrowed local may not be returned from a method at
// all unless the return is a borrow with a well-defined source that outlives
// the call." `a.GetEnumerator()` and `a.AsReadOnlySpan()` are borrows of `a`
// (outside a lambda, `return a.GetEnumerator();` from this method is
// rejected), and through `return f();` such a borrow reaches the caller.
//
// The first #7558 fix lifted the lambda-read escape at EVERY safe-read
// position, so these rows freed the array at `Make`'s scope end and the caller
// read it (heap-use-after-free in `bz$arrenum$...$Current` / the span read).
// The fix-forward lifts it only for a read whose chain reaches a value copy.
//
// The control pins MEMORY SAFETY and the output: it runs under
// `runAotCheckedLenient` (generated-code ASan, LeakSanitizer off), because the
// sound codegen verdict for a borrow that escapes through the lambda's result
// is the conservative one, which keeps (leaks) the array. Red on the first
// fix (b9ebb9728): heap-use-after-free, ran=false.
//
// It is ONE row with TWO spellings (the enumerator and the span), so the
// suite's base-green budget covers both: 100 * (1+2+3) from the enumerator
// plus 100 * (1+3) from the span = 1000.
// ---------------------------------------------------------------------------

namespace {

std::string lenientOut(const std::string& program) {
    auto r = runAotCheckedLenient(program);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// rev-s55 p27 (an enumerator over the captured array, returned through `f()`)
// and rev-s55 p42 (a read-only span over the captured array, returned through
// `f()`), in one program.
BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           boundary_control_lambda_returns_borrow_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method MakeEnum(): IEnumerator<int>
    var a: Array<int> := [1,2,3];
    var f := lambda() => a.GetEnumerator();
    return f();
  end MakeEnum;
  static method MakeSpan(): ReadOnlyArraySpan<int>
    var a: Array<int> := [1,2,3];
    var f := lambda() => a.AsReadOnlySpan();
    return f();
  end MakeSpan;
  public static method Main(): int
    var s := 0;
    for var i in 1..100 do
      var e := MakeEnum(); while e.MoveNext() do s := s + e.Current; end;
      var sp := MakeSpan(); s := s + sp[0] + sp[2];
    end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "1000\n");
}

// ---------------------------------------------------------------------------
// BOUNDARY CONTROL 2 of 2 (owner cap 2026-11-12): a `string` step is not a
// value copy.
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
// Each spelling returns, through `return f();`, a span over a DYNAMIC element
// string (`$"q{n}zzz"`), and the caller reads it 200 times. The control pins
// MEMORY SAFETY and the output under `runAotCheckedLenient` (ASan on, LSan
// off): the sound codegen verdict here is the conservative one, which keeps
// (leaks) the array. Red on the first fix-forward (b39d36f6d): SEGV,
// ran=false.
//
// It is ONE row with SEVEN spellings — the index read, the view-alias `Last`,
// the generic identity, `ElementAt`, the `for`-in arm, the block-local
// binding and the sub-span — so the suite's base-green budget covers them
// all: the six spellings that print a whole element's length each contribute
// 1292 over the 200 iterations, and the sub-span contributes 400:
// 1292 * 6 + 400 = 8152.
// ---------------------------------------------------------------------------

BEZEL_TEST(captured_array_literal_lambda_leak_7558,
           boundary_control_lambda_returns_string_element_span_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method Id<T>(x: T): T => x;
  static method MakeIndex(n: int): ReadOnlyStringSpan
    var a: Array<string> := ["ab", $"q{n}zzz", "c"];
    var f := lambda() => a[1].AsSpan();
    return f();
  end MakeIndex;
  static method MakeViewAliasLast(n: int): ReadOnlyStringSpan
    var a: Array<string> := ["ab", "c", $"q{n}zzz"];
    var c: Array<string>? := a;
    var f := lambda() => c!.Last().AsSpan();
    return f();
  end MakeViewAliasLast;
  static method MakeGenericId(n: int): ReadOnlyStringSpan
    var a: Array<string> := ["ab", $"q{n}zzz", "c"];
    var f := lambda() => Id(a[1]).AsSpan();
    return f();
  end MakeGenericId;
  static method MakeElementAt(n: int): ReadOnlyStringSpan
    var a: Array<string> := ["ab", $"q{n}zzz", "c"];
    var f := lambda() => a.ElementAt(1).AsSpan();
    return f();
  end MakeElementAt;
  static method MakeForIn(n: int): ReadOnlyStringSpan
    var a: Array<string> := [$"q{n}zzz", "c"];
    var f := lambda(): ReadOnlyStringSpan => do for var x in a do return x.AsSpan(); end; return "".AsSpan(); end;
    return f();
  end MakeForIn;
  static method MakeBlockLocal(n: int): ReadOnlyStringSpan
    var a: Array<string> := ["ab", $"q{n}zzz", "c"];
    var f := lambda(): ReadOnlyStringSpan => do var s := a[1]; return s.AsSpan(); end;
    return f();
  end MakeBlockLocal;
  static method MakeSubSpan(n: int): ReadOnlyStringSpan
    var a: Array<string> := ["ab", $"q{n}zzz", "c"];
    var f := lambda() => a[1].AsSpan(1, 2);
    return f();
  end MakeSubSpan;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do
      var sp1 := MakeIndex(i); s := s + sp1.ToString().Length;
      var sp2 := MakeViewAliasLast(i); s := s + sp2.ToString().Length;
      var sp3 := MakeGenericId(i); s := s + sp3.ToString().Length;
      var sp4 := MakeElementAt(i); s := s + sp4.ToString().Length;
      var sp5 := MakeForIn(i); s := s + sp5.ToString().Length;
      var sp6 := MakeBlockLocal(i); s := s + sp6.ToString().Length;
      var sp7 := MakeSubSpan(i); s := s + sp7.ToString().Length;
    end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "8152\n");
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
