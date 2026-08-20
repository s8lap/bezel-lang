// Spec-derived regression tests for issue #2641 — a PLAIN inline `out var`
// recipient of `string` type was never released at scope exit, leaking one heap
// string per call.
//
// WHAT THE DEFECT WAS. `ensureOutVarSlots`
// (`src/codegen/LlvmBackend_Calls2.cpp`) creates the caller-side slot for each
// inline out-var argument. Its `out owned var` and `out using var` arms each
// register a scope-end `disposeStack_` entry for the recipient; the PLAIN
// `out var` arm fell through to `locals_[id] = slot;` and registered nothing at
// all. The recipient therefore had no discharge on any path.
//
// WHY THAT IS A LEAK OF A REFERENCE THE CALLER OWNS. §22.3 "Parameter
// modifiers" (Bezel_Language_Spec.md v0.268) specifies `out` as
// "copy-out-on-normal-return: within the callee the parameter denotes a
// callee-local slot, freely readable after assignment; on normal return the
// slot's value is copied to the caller's recipient", and blesses this backend's
// implementation of it: "An implementation may pass a hidden reference". The
// callee's `n := <expr>` therefore stores through the CALLER's slot, and that
// store goes through `lowerStringRefParamStore` (#2321,
// `LlvmBackend_Setup.cpp`), which emits the §28.9 balance:
//
//     load-old → retain-new (or TRANSFER a +1 producer) → store-new → release-old
//
// So on return the caller's slot holds exactly ONE reference that no other party
// holds — a +1 minted or retained specifically for it. §28.9 "Runtime-managed
// types" puts `string` among the types whose "lifetime is managed by the runtime
// without user involvement", so reclaiming that reference is the
// IMPLEMENTATION's obligation, not the program's; this backend discharges it by
// refcount, and the scope-end release is the discharge every other string local
// already gets (`Disposal::stringRelease`, #422). Without it the reference is
// simply never decremented.
//
// The EXCEPTION edge is not this fix's business and is not tested here: §22.3
// continues "on an exception exit the recipient is untouched — an inline
// `out var` recipient stays unassigned", so the slot still holds the null
// `ensureOutVarSlots` stored and the release is a no-op on that edge.
//
// The leak was MEASURED, not inferred, before this fix existed: #2629's own loop
// test comment records "pre-fix (no store at all) 39 B in 2 allocations … The
// residual 39 B is a PRE-EXISTING leak of the FINAL iteration's value at scope
// exit — a plain `out var` string recipient is never released when its block
// ends … filed separately." This file is that separate filing's regression net,
// and `loop_reused_out_var_slot_is_leak_free` is #2629's own program with the
// oracle upgraded from "the value is right" to "and nothing leaked".
//
// ⚠ THE ORACLE HERE IS LEAKSANITIZER, NOT THE EXIT CODE. Every test that pins
// the leak uses `runAotChecked` (generated-code ASan + LSan). On the PLAIN build
// these programs return the right value both before and after the fix — the
// defect is invisible to a value oracle. Conversely #2629, whose defect was a
// SIGSEGV on the plain build, is masked under ASan. Neither oracle dominates
// (#2633); this file needs the LSan one.
//
// WHY THE FIX IS GATED AND NOT UNCONDITIONAL. A scope-end release is only
// balanced while the caller's slot is the SOLE owner. Two of the gates are
// pinned here as behavior:
//
//   * ESCAPE. If the recipient is returned, stored into a field, or captured,
//     an alias outlives the scope and a release would dangle it. The fix runs
//     the same conservative `StringEscapeWalker` a `var` string local gets; an
//     escaping recipient is left un-released (it keeps leaking — the deliberate
//     leak-over-UAF direction). `returned_out_var_recipient_is_not_released` and
//     `field_stored_out_var_recipient_is_not_released` would report a
//     use-after-free / double-free under ASan if that gate were dropped.
//
//   * NULLABLE. `lowerStringRefParamStore` bails on a nullable target, so an
//     `out n: string?` slot receives a plain store that carries NO reference for
//     the caller; releasing it would be an over-release of the callee's value.
//     The fix mirrors that helper's surface exactly, so a `string?` recipient
//     stays un-released (and stays leaking). `nullable_out_var_recipient_scope_
//     boundary` pins that as a deliberate scope line rather than an oversight —
//     it asserts the VALUE on the plain oracle, because the LSan oracle would
//     (correctly) still find that leak.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ASan + LSan oracle: a leak / double-free / UAF report at exit surfaces as
// ran=false, which this flips to the -999 sentinel.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// Plain (non-sanitized) oracle — value only.
int run(const std::string& src) {
    JitResult r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// -----------------------------------------------------------------------------
// #2641's own repro shape: one call, one heap string copied out, block ends.
// The callee's `s + "-tail"` is a +1 producer whose reference #2321 TRANSFERS
// into the caller's slot, so after the call the caller's slot is that string's
// only owner. Pre-fix nothing released it → LSan reports the leak → -999.
// Post-fix the scope-end release balances it and Main returns 6.
// per §22.4, §22.3, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_scope_disposal_2641,
           single_call_out_var_string_recipient_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var b := Widget.Make(\"x\", out var n); "
        "    _ := b; "
        "    return n.Length; "
        "  end Main; end C;"), 6);
}

// -----------------------------------------------------------------------------
// #2629's LOOP program verbatim, with the oracle upgraded from value to LSan.
// This is the exact measurement that issue records (39 B in 2 allocations on a
// 5-iteration loop). It is also the shape that makes the fix's slot-NULLING
// load-bearing: the recipient's alloca is entry-block hoisted, so iteration N+1's
// callee store LOADS the slot to release what it displaces. If the scope-end
// release did not also null the slot, that load would see the pointer this
// iteration just freed — a double release, which ASan reports rather than
// tolerating. So this test fails BOTH ways: leak without the release,
// double-free without the null.
// per §22.4, §22.3, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_scope_disposal_2641,
           loop_reused_out_var_slot_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 5 do "
        "      var b := Widget.Make(\"x\", out var n); "
        "      _ := b; "
        "      total := total + n.Length; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; end C;"), 30);
}

// -----------------------------------------------------------------------------
// The callee assigns a BORROW (its own `string` parameter), not a producer.
// #2321 RETAINS in that case rather than transferring, so the caller's slot again
// owns exactly one independent reference — and the argument's own reference is
// still the caller's literal, untouched. A release that assumed "producer only"
// would under-release here (leak) and one that released the ARGUMENT would
// over-release it; both are caught.
// per §22.3, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_scope_disposal_2641,
           borrow_assigned_out_var_recipient_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public static method Echo(s: string, out n: string): bool "
        "    n := s; return true; "
        "  end Echo; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var src := \"ab\" + \"cde\"; "
        "    var b := Widget.Echo(src, out var n); "
        "    _ := b; "
        "    return n.Length + src.Length; "
        "  end Main; end C;"), 10);
}

// -----------------------------------------------------------------------------
// The callee assigns TWICE. The second store's release-of-old reclaims the first
// value, so the caller's slot still owns exactly one reference at return — the
// scope-end release must fire once, not once per assignment.
// per §22.3, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_scope_disposal_2641,
           twice_assigned_out_var_recipient_is_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-one\"; "
        "    n := s + \"-second\"; "
        "    return true; "
        "  end Make; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var b := Widget.Make(\"x\", out var n); "
        "    _ := b; "
        "    return n.Length; "
        "  end Main; end C;"), 8);
}

// -----------------------------------------------------------------------------
// TWO inline out-var recipients in ONE call. Each gets its own slot and its own
// registration; a fix that keyed the disposal on the call rather than on the
// argument would release one twice and the other never.
// per §22.4, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_scope_disposal_2641,
           two_out_var_recipients_in_one_call_are_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public static method Split(s: string, out a: string, "
        "                             out b: string): bool "
        "    a := s + \"-a\"; b := s + \"-bb\"; return true; "
        "  end Split; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var ok := Widget.Split(\"x\", out var a, out var b); "
        "    _ := ok; "
        "    return a.Length + b.Length; "
        "  end Main; end C;"), 7);
}

// -----------------------------------------------------------------------------
// ESCAPE GUARD — the recipient is RETURNED out of the declaring method. The
// scope-end release must NOT fire: the caller of `Wrap` reads the string after `Wrap`'s
// scope has ended, so a scope-end release there would hand back freed memory.
// The fix's escape walk excludes the recipient, leaving the pre-existing leak.
// Run on the PLAIN oracle: the leak is real and deliberate (leak-over-UAF), so
// the LSan oracle would fail on the very behavior this test is asserting. What
// it pins is that the VALUE survives — with the escape gate removed, this reads
// a released string.
// per §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_scope_disposal_2641,
           returned_out_var_recipient_is_not_released) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end Widget; "
        "class W "
        "  public static method Wrap(): string "
        "    var b := Widget.Make(\"x\", out var n); "
        "    _ := b; "
        "    return n; "
        "  end Wrap; "
        "end W; "
        "class C "
        "  public static method Main(): int "
        "    var got := W.Wrap(); "
        "    return got.Length; "
        "  end Main; end C;"), 6);
}

// -----------------------------------------------------------------------------
// ESCAPE GUARD — the recipient is stored into a CLASS FIELD, which outlives the
// declaring scope and is released by the owner's disposal (§28.9). Same reason
// as above: releasing at scope end would be a second release of a value the
// field still holds. Plain oracle, for the same reason.
// per §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_scope_disposal_2641,
           field_stored_out_var_recipient_is_not_released) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end Widget; "
        "class Holder "
        "  public kept: string := \"\"; "
        "  public constructor() end; "
        "  public method Fill() "
        "    var b := Widget.Make(\"x\", out var n); "
        "    _ := b; "
        "    kept := n; "
        "  end Fill; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    h.Fill(); "
        "    return h.kept.Length; "
        "  end Main; end C;"), 6);
}

// -----------------------------------------------------------------------------
// SCOPE BOUNDARY — a NULLABLE `out n: string?` recipient. `lowerStringRefParamStore`
// declines a nullable target, so the callee's store leaves the caller's slot with
// no reference of its own; the fix mirrors that surface and registers nothing.
// This is a deliberate scope line (documented at `registerOutVarStringRelease`),
// not an oversight: pinning it means a later widening of the fix must widen
// #2321 in the same commit or this test's plain-oracle value turns into a
// use-after-free. Plain oracle: the recipient's value, unchanged by this fix.
// per §22.3, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_scope_disposal_2641,
           nullable_out_var_recipient_scope_boundary) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public static method Make(s: string, out n: string?): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var b := Widget.Make(\"x\", out var n); "
        "    _ := b; "
        "    if n <> null then return n.Length; end; "
        "    return 0; "
        "  end Main; end C;"), 6);
}

// -----------------------------------------------------------------------------
// CONTROL — a PRIMITIVE `out var` recipient carries no reference at all and must
// gain no disposal. Guards against a widening that registered a
// `bz_string_release` on an `int` slot (which would read the integer as a
// pointer). Value + LSan.
// per §22.4, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_scope_disposal_2641,
           primitive_out_var_recipient_control_unaffected) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class Widget "
        "  public static method Make(s: string, out n: int): bool "
        "    n := 7; return true; "
        "  end Make; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var b := Widget.Make(\"x\", out var n); "
        "    _ := b; "
        "    return n; "
        "  end Main; end C;"), 7);
}

// -----------------------------------------------------------------------------
// CONTROL — a PRE-DECLARED (non-inline) `out n` recipient never reaches
// `ensureOutVarSlots`' inline arm at all: its slot already exists (created by its
// own `LocalDecl`), so the arm's "skip if slot already created" guard fires, and
// its call-site modifier is `out`, not `out var`. It is therefore structurally
// outside this fix.
//
// ⚠ IT LEAKS THE SAME 39 B, and that is a SEPARATE PRE-EXISTING DEFECT, measured
// here rather than assumed. #2641 listed "whether a pre-declared (non-inline)
// `out n: string` recipient leaks the same way at scope exit" among what it did
// NOT verify; `scripts/agent_check.sh --probe --asan` on this exact program
// reports "39 byte(s) leaked in 2 allocation(s)" from `bz$M.Widget$Make` — the
// same signature as the inline case. The cause is different: the pre-declared
// recipient IS an ordinary `var` string local, and the existing escape walk
// treats an `out`-modified argument as a conservative escape, so
// `armStringLocalRefcount` never arms its scope-end release. Fixing it means
// teaching that walk about the `out` position, not touching this file's arm —
// filed separately.
//
// So this control asserts on the PLAIN oracle. Its job is to catch a fix that
// over-reached into the pre-declared shape and registered a SECOND release on a
// slot whose own machinery may already own one — a double-free. Upgrade it to
// `runChecked` when the separate defect lands.
// per §22.3, §28.9
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_scope_disposal_2641,
           predeclared_out_recipient_control_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var n: string := \"zz\"; "
        "    var b := Widget.Make(\"x\", out n); "
        "    _ := b; "
        "    return n.Length; "
        "  end Main; end C;"), 6);
}
