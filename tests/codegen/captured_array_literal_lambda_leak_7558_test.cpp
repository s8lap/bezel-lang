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
// Each program calls the leaking method 100 times, so a leak is 100 allocations
// and cannot hide behind a stale stack slot (a single leak of this shape is
// reported by LeakSanitizer only intermittently). Rows run under
// `runAotChecked` (AddressSanitizer + LeakSanitizer) and compare stdout.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

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
