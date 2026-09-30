// Codegen / disposal — fix #2992: THE BEHAVIOURAL HALF of the `using var`
// auto-drop chain guard (language spec §28.3 "Local variables, ownership, and
// borrows" / §28.9 "Runtime-managed types", v0.254).
//
// #2992's failure mode was SILENCE: when no arm in registerUsingDisposal's
// ~25-arm chain claimed a built-in, the scope-exit path emitted nothing at all
// and said nothing — the structural defect behind #2921 (Random leaked 3200
// bytes in 200 allocations under LSan) and #2973 (Atomic / AtomicInteger, 4800
// bytes in 200 allocations each). The fix added an end-of-chain guard that
// emits an `internal:` note when such a built-in falls through every arm.
//
// ── WHY THIS FILE WAS REWRITTEN (#3138) ────────────────────────────────────
//
// Its previous seven tests were ALL of the shape
//
//     auto r = runAot(usingVarProg("using var x := new Channel<int>();"));
//     BEZEL_EXPECT_TRUE(r.ran);
//     BEZEL_EXPECT_FALSE(hasChainGuardNote(r));   // scans r.diagnostics
//
// and every one of them was VACUOUS. `runAot` populates
// `JitResult::diagnostics` only on a compile/link reject, a compiler crash or
// timeout, a fired sanitizer report, or a signal — and each of those paths
// returns BEFORE `r.ran = WIFEXITED(status)` (tests/support/aot_support.hpp). So
// `EXPECT_TRUE(r.ran)` already implies `r.diagnostics` is EMPTY, and the second
// assertion could not fail on any compiler, fixed or broken. #2992's entire
// regression coverage was fictional, and the file's "HOW TO VERIFY
// FAILS-WITHOUT" recipe — remove the Random arm and watch the note appear —
// could not reproduce through this harness at all.
//
// The coverage is now split across the two tracks by what each can actually
// observe:
//
//   * THE DIAGNOSTIC ORACLE moved to lit, where a real `bezelc` run and
//     FileCheck DO see a Warning on a program that compiles:
//     tests/lit/using_var_autodrop_chain_guard_2992.bzl.
//   * THE BEHAVIOURAL ORACLE is this file, and it is a strictly stronger claim
//     than the note ever was: the note says "an arm claimed it", while these
//     tests say "the object was actually RELEASED". `runAotChecked` compiles the
//     generated code with AddressSanitizer and links `-fsanitize=address`;
//     LeakSanitizer forces a non-zero exit on any leak and the harness turns its
//     report into `ran == false`. So an arm that stops matching — or one that
//     matches and emits the WRONG release — fails here, which the note-based
//     test could not have caught even if it had been able to fail.
//
// PROVEN FAILS-WITHOUT (mechanically, on this base): deleting
// `if (n == "Random") return "Random";` from
// `Emitter::usingAutoDropVtablelessBuiltin`
// (src/codegen/LlvmBackend_Disposal.cpp) turns `random_using_var_is_released`
// RED — LSan reports the leak and `r.ran` goes false — and restoring the line
// turns it green again. The same edit turns the lit sibling red on the note.
//
// LOOP-AMPLIFIED, deliberately: a one-shot `Main` is not a reliable LSan oracle,
// because a leftover pointer in a register makes a leaked block read as merely
// reachable rather than lost. 200 iterations is the count at which #2921 and
// #2973 were originally measured, so a regression reproduces the filed numbers.
//
// Each local is USED, not merely bound — an unused `using var` risks the §28.10
// vacuous-pin lint, and a probe whose object is never touched is a weaker
// exercise of the arm than one whose object is.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A 200-iteration `using var` loop over one built-in. `body` binds and USES the
// built-in, accumulating into `acc`; `expected` is the exact total, so a "fix"
// that stopped the loop doing its work would not pass either.
std::string loopProg(const std::string& body, int expected) {
    return "module M; import System.Diagnostics; class C "
           "  public static method Main(): int "
           "    var acc: int := 0; "
           "    var i: int := 0; "
           "    while i < 200 do "
           "      i := i + 1; " + body +
           "    end while; "
           "    if acc <> " + std::to_string(expected) + " then return 3; end if; "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The verdict, spelled once: under runAotChecked, `ran == false` is what a
// LeakSanitizer report looks like from the harness's side, and a surviving
// program must also have returned 0 (a 3 would mean the loop's own arithmetic
// changed rather than its disposal).
void expectNoLeak(const bezel::test::JitResult& r) {
    BEZEL_EXPECT_TRUE(r.ran);      // false <- LSan fired (leak / UAF / double free)
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

}  // namespace

// ---------------------------------------------------------------------------
// One test per chain-claimed disposable built-in. Each is claimed by a dedicated
// arm in registerUsingDisposal (or, for the three vtable-less names, by
// usingAutoDropVtablelessBuiltin + emitVtablelessBuiltinRelease); removing that
// arm reinstates exactly the leak the issue measured.
// per §28.3 / §28.9
// ---------------------------------------------------------------------------

// stdlib §6.9 — Channel<T>.
BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           channel_using_var_is_released_spec_28_3) {
    auto r = runAotChecked(loopProg(
        "using var ch := new Channel<int>(); "
        "acc := acc + ch.Count + 1; ", 200));
    expectNoLeak(r);
}

// stdlib §3.1 — StringBuilder: released through the borrowing-`Array<string>`
// free with the string-element release thunk (#1185), so a per-part `+1` that
// the thunk failed to discharge would also surface here.
BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           stringbuilder_using_var_is_released_spec_28_3) {
    auto r = runAotChecked(loopProg(
        "using var sb := new StringBuilder(); "
        "_ := sb.Append(\"x\"); "
        "acc := acc + sb.Length; ", 200));
    expectNoLeak(r);
}

// stdlib §6.2 — Mutex, plus the `owned Lock` its Acquire mints (two nested
// obligations released in reverse declaration order).
BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           mutex_and_lock_using_var_are_released_spec_28_3) {
    auto r = runAotChecked(loopProg(
        "using var mu := new Mutex(); "
        "using var g := mu.Acquire(); "
        "acc := acc + 1; ", 200));
    expectNoLeak(r);
}

// stdlib §2.7 — Random. THE #2921 CASE: its `bz_random_free` was declared in the
// runtime header and referenced by no emitter at all, so `using var` on a
// Random leaked 3200 bytes in 200 allocations. Its Dispose is a documented
// no-op, but §28.9's trivial-dispose rule still reclaims the INSTANCE at its
// owner's release point — which is precisely what this measures.
BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           random_using_var_is_released_spec_28_3_28_9) {
    auto r = runAotChecked(loopProg(
        "using var rnd := new Random(); "
        "acc := acc + rnd.Next(1) + 1; ", 200));   // Next(1) is 0..<1, always 0
    expectNoLeak(r);
}

// stdlib §6.4 — Atomic<T>. THE #2973 CASE: 4800 bytes in 200 allocations.
BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           atomic_using_var_is_released_spec_28_3_28_9) {
    auto r = runAotChecked(loopProg(
        "using var at := new Atomic<int>(5); "
        "acc := acc + at.Value; ", 1000));
    expectNoLeak(r);
}

// stdlib §6.4 — AtomicInteger<T>, the other half of #2973 (a separate class,
// not a subtype of Atomic<T>, hence a separate arm entry and a separate probe).
BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           atomic_integer_using_var_is_released_spec_28_3_28_9) {
    auto r = runAotChecked(loopProg(
        "using var ai := new AtomicInteger<int>(1); "
        "acc := acc + ai.Increment(); ", 400));
    expectNoLeak(r);
}

// stdlib §8.1 — Stopwatch.
BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           stopwatch_using_var_is_released_spec_28_3) {
    auto r = runAotChecked(loopProg(
        "using var sw := new Stopwatch(); "
        "sw.Start(); "
        "sw.Stop(); "
        "if sw.IsRunning then return 4; end if; "
        "acc := acc + 1; ", 200));
    expectNoLeak(r);
}

// stdlib §6.5 — EventSource<T>: released via bz_event_source_free (#2026).
BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           event_source_using_var_is_released_spec_28_3) {
    auto r = runAotChecked(loopProg(
        "using var es := new EventSource<int>(); "
        "es.Raise(3); "
        "acc := acc + 1; ", 200));
    expectNoLeak(r);
}
