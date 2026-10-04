// Regression tests for issue #5231 (§28.9 / §28.14 / §28.11, v0.313).
//
// The #5216 fix added calleeBodyUsesFormalOnlyByInvocation to prevent releasing
// a lambda when the callee body uses the formal in a non-invocation way (e.g.
// storing it in a collection). However, this check was only applied to the
// call-bounded arm (LlvmBackend_Calls2.cpp line 13727-13729), not to the
// sync-iterator arm (line 13459).
//
// For a sync iterator callee, the lambda was still being added to
// syncIterClosureArgs and released at the call end (line 13934), even when
// the callee body stored the lambda in a collection. This caused a
// heap-use-after-free when the stored lambda was later invoked.
//
// The fix applies the same calleeBodyUsesFormalOnlyByInvocation check to the
// sync-iterator path.
//
// Oracles. The storing rows use runAotCheckedLenient: ASan (the UAF /
// double-free detector) stays ON, only LeakSanitizer is off, because the
// stored cell is LEAKED (same as the non-generic
// `Array<() -> int>.Add(lambda () => 42)` path per §28.11's
// runtime-managed element row is implemented for `string` only). Each row
// asserts the program's VALUE on stdout (the invocations through the stored
// cells), so "the cell survived intact" is checked, not just "nothing
// crashed". The control row (callee never references `x`) is under
// runAotChecked: the release must still fire there and the program must be
// leak-free.
//
// Without the fix, `lambda_literal_stored_in_sync_iter_yield` dies under
// ASan with heap-use-after-free.
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
// 200 stored cells, each answering 42, invoked once after the loop.
constexpr const char* kSum = "8400\n";
}  // namespace

// Issue #5231's literal repro (r14_iter.bzl), looped 200×: the literal is
// passed to a sync iterator method's bare-U parameter, stored into the
// caller's Array via l.Add(x), and invoked after every call has returned.
BEZEL_TEST(codegen_5231, lambda_literal_stored_in_sync_iter_yield) {
    auto r = runAotCheckedLenient(
        "module R14; "
        "class H "
        "  public static method It<U>(l: Array<U>, x: U): IEnumerable<int> "
        "    l.Add(x); "
        "    yield return 1; "
        "  end It; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    var keep := new Array<() -> int>(); "
        "    var i := 0; "
        "    while i < 200 do "
        "      for var n in H.It<() -> int>(keep, lambda () => 42) do "
        "        i := i + n; "
        "      end; "
        "    end; "
        "    var s := 0; "
        "    for var g in keep do "
        "      s := s + g(); "
        "    end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, kSum);
}

// Control: the sync iterator never references `x`, so the call-end release
// must still fire and the program must be leak-free (runAotChecked, LSan on).
// A named local is passed so the temporary rules for deferred-activation callees
// are satisfied.
BEZEL_TEST(codegen_5231,
           lambda_literal_unreferenced_formal_in_sync_iter_released_at_call_end) {
    auto r = runAotChecked(
        "module Conf; "
        "class H "
        "  public static method It<U>(l: Array<U>, x: U): IEnumerable<int> "
        "    yield return 1; "
        "  end It; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var arr := new Array<() -> int>(); "
        "      for var n in H.It<() -> int>(arr, lambda () => 42) do "
        "        total := total + n; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// Store via Dictionary in a sync iterator.
BEZEL_TEST(codegen_5231, lambda_literal_stored_in_sync_iter_via_dictionary) {
    auto r = runAotCheckedLenient(
        "module Conf; "
        "class H "
        "  public static method It<U>(d: Dictionary<int, U>, k: int, x: U): IEnumerable<int> "
        "    d.Add(k, x); "
        "    yield return 1; "
        "  end It; "
        "end H; "
        "class Program "
        "  public static method Main(): int "
        "    var keep := new Dictionary<int, () -> int>(); "
        "    var i := 0; "
        "    while i < 200 do "
        "      for var n in H.It<() -> int>(keep, i, lambda () => 42) do "
        "        i := i + n; "
        "      end; "
        "    end; "
        "    var s := 0; "
        "    for var e in keep do "
        "      s := s + e.Value(); "
        "    end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, kSum);
}
