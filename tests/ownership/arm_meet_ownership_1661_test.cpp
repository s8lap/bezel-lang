// Disposal-correctness — the §28.3 arm-meet ownership of an if-expression
// (`v1 if c else v2`) and an expression-position `case`/`when`, used as an owned
// initializer (issue #1661; spec §28.3 producers row, v0.184/v0.196; §20; §19.2).
//
// §28.3 (v0.184): the ownership of `v1 if c else v2` and of an expression-
// position `case`/`when` is the MEET of the arms' ownership states —
// "owned⊓owned = owned — exactly one arm's value is produced at runtime and is
// discharged once, so there is no leak and no double-free; borrowed⊓borrowed =
// borrowed ... a mixed owned/borrowed arm set is rejected". §28.10 admits such
// an owned-meet expression as an owned `using var` initializer, with the spec's
// own canonical `using var x := new Buffer() if useFresh else source.Clone-
// Buffer();` example (CloneBuffer(): owned Buffer).
//
// THE BUG (#1661). The ownership pass did not classify a ConditionalExpr /
// CaseExpr initializer under the arm-meet rule, and codegen's disposal mirrors
// (usingVarOwnsClassObjectDynamically / unwindOwnedLocalClass) did not recognize
// it either. Consequences, all confirmed via native AOT + ASan/LSan on HEAD:
//   (1) an owned-meet `using var`/`var` was misclassified a borrow → the fresh
//       owned value was NEVER disposed — a direct leak (destructor never ran);
//   (2) a TYPED borrowed-meet `using var x: Res := b1 if c else b2;` reached
//       classOfLocal (which returns the declared class regardless of ownership)
//       and DISPOSED the borrow at scope end — a double-free (heap-use-after-
//       free) of the value its real owner also disposes.
//
// THE FIX. common::conditionalCaseArmMeetIsOwned is the single shared predicate
// the ownership pass (producerState) and both codegen mirrors consult, so they
// never diverge: an owned-meet if/case is disposed EXACTLY once; a borrowed-meet
// pins (usingVarSourceIsBorrow) and is never disposed here; a mixed set is
// rejected in sema (the lit siblings under tests/lit/ownership/).
//
// OBSERVABILITY. Each leak (1) is a DIRECT leak and each double-free (2) a heap-
// use-after-free — runAotChecked's LSan/ASan flips r.ran to false WITHOUT the
// fix. The destructor also prints a per-instance marker, so the count pins
// dispose-exactly-once. This is disposal-correctness, so the accept cases MUST
// use runAotChecked (per-program ASan/LSan), per tests/README.md.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

const char* kResClass =
    "module M; "
    "class Res : IDisposable "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "end Res; "
    "class W "
    "  public static method Make(id: int): owned Res return new Res(id); "
    "  end Make; ";

// (1) if-expression, owned⊓owned, untyped `using var`. useFresh=true selects the
// `new Res(1)` arm; it is disposed once at scope end. The spec's canonical
// Pattern B shape (§28.10). Without the fix: never disposed → LSan direct leak.
std::string kIfExprOwnedMeetUsingVar =
    std::string(kResClass) +
    "  public static method Main(): int "
    "    var useFresh := true; "
    "    using var x := (if useFresh then new Res(1) else Make(2)); "
    "    Console.WriteLine(\"body\"); "
    "    return 0; "
    "  end Main; "
    "end W;";

// (2) expression-position `case`/`when`, owned⊓owned, untyped `using var`. n=2
// selects the `_ => Make(7)` arm (an owned-returning call); disposed once.
std::string kCaseExprOwnedMeetUsingVar =
    std::string(kResClass) +
    "  public static method Main(): int "
    "    var n := 2; "
    "    using var x := (case n of 1 => new Res(1), _ => Make(7)); "
    "    Console.WriteLine(\"body\"); "
    "    return 0; "
    "  end Main; "
    "end W;";

// (3) TYPED borrowed⊓borrowed `using var`. Both arms borrow the owned `owner`
// (b1/b2 alias it), so the meet is a borrow — the using var is a PIN and must
// NOT dispose. Only `owner` is disposed, once. Without the fix the typed slot's
// classOfLocal disposed the borrow too → a heap-use-after-free double-free of
// Res(7) (r.ran false under ASan).
std::string kTypedBorrowedMeetUsingVarPins =
    std::string(kResClass) +
    "  public static method Main(): int "
    "    var c := true; "
    "    using var owner := new Res(7); "
    "    var b1 := owner; "
    "    var b2 := owner; "
    "    using var x: Res := (if c then b1 else b2); "
    "    Console.WriteLine(\"body\"); "
    "    return 0; "
    "  end Main; "
    "end W;";

// (4) plain `var` owned⊓owned, discharged with an explicit `Dispose()`. The
// value must be disposed AND reclaimed exactly once. Without the fix
// unwindOwnedLocalClass did not recognize the conditional, so the explicit
// Dispose ran the destructor but never reclaimed the block → LSan direct leak.
std::string kPlainVarOwnedMeetExplicitDispose =
    std::string(kResClass) +
    "  public static method Main(): int "
    "    var c := true; "
    "    var x := (if c then new Res(1) else Make(2)); "
    "    x.Dispose(); "
    "    Console.WriteLine(\"body\"); "
    "    return 0; "
    "  end Main; "
    "end W;";

}  // namespace

// ===========================================================================
// (1) if-expression owned-meet `using var` — disposed EXACTLY once, no leak.
// ===========================================================================
BEZEL_TEST(ownership_arm_meet_1661,
           if_expr_owned_meet_using_var_disposes_once_spec_28_3) {
    JitResult r = runAotChecked(kIfExprOwnedMeetUsingVar);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // WITHOUT the fix: LSan direct leak (never disposed) → r.ran false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The taken arm's value disposed exactly once; the untaken arm never runs.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 0);
    // Scope-end disposal: the destructor runs after the body statement.
    const std::size_t body = r.stdoutText.find("body");
    const std::size_t disp = r.stdoutText.find("D1");
    BEZEL_EXPECT_TRUE(body != std::string::npos);
    BEZEL_EXPECT_TRUE(disp != std::string::npos);
    BEZEL_EXPECT_TRUE(body < disp);
}

// ===========================================================================
// (2) expression-position case/when owned-meet `using var` — disposed once.
// ===========================================================================
BEZEL_TEST(ownership_arm_meet_1661,
           case_expr_owned_meet_using_var_disposes_once_spec_28_3) {
    JitResult r = runAotChecked(kCaseExprOwnedMeetUsingVar);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D7"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 0);
}

// ===========================================================================
// (3) TYPED borrowed-meet `using var` PINS — the borrow is never disposed
// here; only the owner is disposed, once (no double-free).
// ===========================================================================
BEZEL_TEST(ownership_arm_meet_1661,
           typed_borrowed_meet_using_var_pins_no_double_free_spec_28_3) {
    JitResult r = runAotChecked(kTypedBorrowedMeetUsingVarPins);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // WITHOUT the fix: heap-use-after-free double-free of Res(7) → r.ran false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Res(7) — the sole owner — disposed EXACTLY once (not twice).
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D7"), 1);
}

// ===========================================================================
// (4) plain `var` owned-meet + explicit Dispose — disposed AND reclaimed once.
// ===========================================================================
BEZEL_TEST(ownership_arm_meet_1661,
           plain_var_owned_meet_explicit_dispose_no_leak_spec_28_3) {
    JitResult r = runAotChecked(kPlainVarOwnedMeetExplicitDispose);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // WITHOUT the fix: destructor ran but the block was never reclaimed → LSan
    // direct leak → r.ran false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 0);
}
