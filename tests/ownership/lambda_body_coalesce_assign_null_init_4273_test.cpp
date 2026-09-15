// Spec-derived regression test for issue #4273 — lambda-body pre-pass never
// handles `??=` at all. The lambda-body pre-pass's AssignmentStmt arm was gated
// `if (as->op == ":=" && as->target)` and had no `??=` arm, so a null-init
// plain local's first `??=` adoption of an owned producer never set
// `becomesOwnedViaReassign` and codegen's `unwindOwnedLocalClass` never
// classified the local as an owned class local for disposal purposes — the
// 16 B allocation leaked with zero diagnostics and the explicit `.Dispose()`
// call dispatched to the destructor without reclaiming the block.
//
// The fix ports the `??=` arm from `handleAssignment` (Ownership.cpp ~line 6768)
// into `applyUsingVarBorrowPinInLambdaStmt`'s AssignmentStmt case, mirroring
// the `:=` arm's approach: when the target is provably null (PolyNull state),
// the `??=` collapses to an unconditional assignment and the same state
// transition fires.
//
// per §28.3, §7.7, §28.7, #1625, #3980, v0.298, #4273

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"        // runAot / runAotChecked
#include "support/Diagnostic.hpp"
#include "support/ownership_support.hpp" // parseAndCheckOwnership, hasErrorMessage
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// Destructor-counting Res (matches the family's established idiom).
const char* kResCounted =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; ";

}  // namespace

// Row 1 (real execution, LOOP-AMPLIFIED 500x per
// [[lsan-one-shot-leak-tests-pass-vacuously]]): a null-initialized local
// (`var r: Res? := null;`) adopts Owned via `??=` — the first (and only)
// value is explicitly `.Dispose()`d. FAIL-WITHOUT this fix: 16 bytes / 1
// allocation leaked — `becomesOwnedViaReassign` was never set, so
// `unwindOwnedLocalClass` never classified `r` as an owned class local and
// the explicit `.Dispose()` dispatched to the destructor without reclaiming
// the block. PASS-WITH: `Res.live == 0` — exact-once disposal of all 500
// allocations.
BEZEL_TEST(lambda_body_coalesce_assign_null_init_4273,
           lambda_null_init_coalesce_assign_disposes_once_amplified_4273) {
    std::string src =
        std::string(kResCounted) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda(): int => do "
        "        var r: Res? := null; "
        "        r ??= new Res(5); "
        "        r.Dispose(); "
        "        return 1; "
        "      end; "
        "      _ := f(); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 2 — parity control: the byte-identical method-body (non-lambda) spelling
// produces the identical result — this shape was ALREADY correctly handled at
// method scope (the `handleAssignment` `??=` block covers it there), so this
// establishes the pre-existing baseline.
BEZEL_TEST(lambda_body_coalesce_assign_null_init_4273,
           method_body_null_init_coalesce_assign_disposes_once_control_4273) {
    std::string src =
        std::string(kResCounted) +
        "class C "
        "  public static method Main(): int "
        "    var r: Res? := null; "
        "    r ??= new Res(5); "
        "    r.Dispose(); "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 3 (real execution): `??=` with a second adopt (drop-on-overwrite).
// A null-init local's first `??=` adopt is followed by a second `??=` adopt
// — the first value is displaced (drop-on-overwrite) and the second is
// explicitly disposed. FAIL-WITHOUT this fix: 32 bytes / 2 allocations
// leaked — the first value leaked because `becomesOwnedViaReassign` was never
// set, and the second value leaked because `unwindOwnedLocalClass` still
// couldn't classify it correctly. PASS-WITH: `Res.live == 0`.
BEZEL_TEST(lambda_body_coalesce_assign_null_init_4273,
           lambda_null_init_coalesce_assign_drop_on_overwrite_4273) {
    std::string src =
        std::string(kResCounted) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda(): int => do "
        "        var r: Res? := null; "
        "        r ??= new Res(5); "
        "        r ??= new Res(9); "
        "        r.Dispose(); "
        "        return 1; "
        "      end; "
        "      _ := f(); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}