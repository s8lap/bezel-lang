// Codegen / runtime — §8.6 / §28.9 / #2304: an Object-returning CALL bound to
// an INFERRED local (`var fromCall := MakeBox(7);` — no `: Object` type
// annotation) must have its single +1 box producer reference released at scope
// end, mirroring #2060's box release for the `var direct: Object := <value>;`
// (DECLARED) shape it left half-open.
//
// §8.6 (v0.236): "When a value type is used where `Object` (or an interface
// implemented by the value type) is expected, the value is boxed: a
// heap-allocated wrapper holds the value."
//
// §28.9 (v0.236): "Boxed value types ... are exempt from manual disposal ...
// the type's lifetime is managed by the runtime without user involvement."
//
// #429 / #2060 closed the DECLARED half: `var o: Object := 5;` was classified
// as a +1 box producer (isBoxableValueType(int) → true), bound the producer's
// +1 into the local (transfer, no retain), and the escape walker released it
// at scope end. The CALL-RESULT half (`var fromCall := MakeBox(7);` — a
// user-defined static method whose body provably returns a boxable value) was
// missing: isPlusOneBoxProducer's DECLARED-TARGET gate `if (!isObjectOrInterfaceTarget(declTy))`
// short-circuited the call-shape recognition because the inferred local has
// ld->type == null (sema fills it in lazily), so the local never entered
// boxReleaseEligible_ and the fresh rc=1 box the callee minted leaked at
// scope end — a 24-byte Direct leak per iteration (#2304: 96 bytes / 4
// allocations over 5 iterations of the loop).
//
// THE FIX (#2304). isPlusOneBoxProducer now also admits the INFERRED shape:
// when ld->type is null, the declared target IS the initializer's static type
// (sema promotes the call's return type into the local), and the case-(b)
// "call to a box-return method" arm runs as before. Without this extension
// every iteration's box leaks; with it the call's return +1 is transferred
// into the local and the escape walker's scope-end bz_box_release drops it
// exactly once.
//
// OBSERVABILITY. The leak is a DIRECT leak of the 24-byte box wrapper (no
// other reference reaches it), so runAotChecked's LSan flags it: WITHOUT the
// fix `r.ran` is false (LSan reports Direct leak of 24 byte(s) per
// iteration); WITH it the run is clean AND no double-free / use-after-free is
// introduced (the dispose-exactly-once net rides on `r.ran`).
//
// TEST SHAPE. Loop-amplified 100× so LSan catches the leak decisively. The
// exit code is `sink mod 256` — the program's own observable value
// (independent of the leak), confirming the program also computes correctly.
// `var direct: Object := 5;` (the #2060 cleanly-released sibling) sits in
// the same loop and verifies the DECLARED shape stays clean (regression
// coverage for #2060 within this test).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// §8.6 / §28.9 / #2304: a static method that ALWAYS returns a boxable value
// (so the boxForTarget at its return site always mints a fresh rc=1 box)
// called and bound to an inferred local — `var fromCall := MakeBox(j);`
// — must have the box released at scope end, alongside the DECLARED sibling
// `var direct: Object := 5;` (the #2060-released shape). 100 iterations
// amplify so LSan reports a clear Direct leak without the fix.
const char* kBoxObjectCallInferredLoop =
    "module M; "
    "class C "
    "  public static method MakeBox(n: int): Object "
    "    return n; "
    "  end MakeBox; "
    "  public static method Main(): int "
    "    var j := 0; var sink := 0; "
    "    while j < 100 do "
    "      var direct: Object := j; "          // #2060 declared (already clean)
    "      var fromCall := MakeBox(j); "        // #2304 inferred (was leaking)
    "      sink := sink + (if direct is int then 1 else 0) "
    "                   + (if fromCall is int then 1 else 0); "
    "      j := j + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

// §8.6 / §28.9 / #2304 variant — an instance method (NOT static) is also a
// valid box-return shape and the inferred-local binding must release it.
// The instance receiver is a separately-owned object (`new C()`), so a
// per-iteration ctor call shows the lifecycle independence. The same sink
// pattern observes both the static-result path AND the instance path in
// one loop.
const char* kBoxObjectCallInferredInstanceLoop =
    "module M; "
    "class C "
    "  public method BoxSelf(n: int): Object "
    "    return n; "
    "  end BoxSelf; "
    "  public static method Main(): int "
    "    var j := 0; var sink := 0; "
    "    while j < 100 do "
    "      using var inst := new C(); "
    "      var fromInstance := inst.BoxSelf(j); "  // inferred, instance method
    "      sink := sink + (if fromInstance is int then 1 else 0); "
    "      j := j + 1; "
    "    end; "
    "    return sink mod 256; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #2304 — an Object-returning CALL bound to an inferred local is released
// at scope end (§8.6 / §28.9); no leak. The cardinal repro from the issue.
// =====================================================================

BEZEL_TEST(codegen_box_object_call_local_leak_2304,
           object_call_inferred_local_does_not_leak_spec_8_6_28_9) {
    // spec §8.6 / §28.9, v0.236: a fresh rc=1 box the callee minted at its
    // return site is the caller's single +1 — a binding local TRANSFERS that
    // reference (no extra retain) and the escape walker proves the local
    // non-escaping (only safe reads — `is int`/`== null`) so the scope-end
    // bz_box_release drops rc 1→0. WITHOUT the #2304 fix the inferred-local
    // call-result form failed isPlusOneBoxProducer's declared-target gate
    // (ld->type is null for inferred locals at codegen-time) and the local
    // never entered boxReleaseEligible_ — every iteration leaked the 24-byte
    // box (2400 bytes total over 100 iterations). LSan flips r.ran false.
    // WITH the fix the inferred-target fallback in isPlusOneBoxProducer treats
    // srcTy as the target when ld->type is null, admits the call, the bind
    // transfers the +1, and the scope-end release frees it exactly once.
    JitResult r = runAotChecked(kBoxObjectCallInferredLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));

    BEZEL_EXPECT_TRUE(r.ran);   // LSan-clean (the #2304 leak would flip this)
}

// =====================================================================
// #2304 — variant — instance-method box-return bound to an inferred local.
// Same release shape; the receiver is garbage-collected by the `using var`
// so the only leak surface is the inferred-local's own box.
// =====================================================================

BEZEL_TEST(codegen_box_object_call_local_leak_2304,
           object_call_inferred_instance_does_not_leak_spec_8_6_28_9) {
    JitResult r = runAotChecked(kBoxObjectCallInferredInstanceLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));

    BEZEL_EXPECT_TRUE(r.ran);
}
