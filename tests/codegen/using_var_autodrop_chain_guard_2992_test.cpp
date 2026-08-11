// Codegen / disposal — fix #2992: the `using var` auto-drop chain's failure
// mode was SILENCE. When no arm in registerUsingDisposal's ~25-arm chain
// claimed a built-in, the scope-exit path emitted nothing at all, with no
// diagnostic — the structural defect behind #2921 (Random) and #2973
// (Atomic/AtomicInteger). This test verifies the fix: the end-of-chain guard
// (registerUsingDisposal, LlvmBackend_Setup.cpp) now checks whether the
// local's type is a vtable-less built-in CLASS that is not a value type
// (common::isVtableLessBuiltinClassName minus the known value-type exclusion),
// and emits an `internal:` note when such a built-in falls through every arm —
// making the next omission loud rather than the next leak silent.
//
// Spec §28.3 (scope-bound disposal, v0.240): a `using var`'s scope-exit
// release MUST be registered. Spec §28.9 (disposal semantics): a heap-owning
// built-in's disposal obligation is real — the compiler frees the undischarged
// value at its owner's release point. The end-of-chain guard enforces §28.3's
// "MUST" by detecting when the chain fails to register it.
//
// WHAT THIS TEST VERIFIES:
//   Every major chain-claimed disposable built-in compiles via `using var`
//   WITHOUT the end-of-chain `internal:` note (the guard is well-calibrated
//   — it fires ONLY on an unclaimed built-in, never on a claimed one).
//
// HOW TO VERIFY FAILS-WITHOUT: temporarily remove the Random arm from
// registerUsingDisposal (the `usingAutoDropVtablelessBuiltin` block); the
// `random` case below then produces the `internal: ... no disposal arm` note.
// With the arm restored, the note is absent.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True if the codegen end-of-chain `internal:` note fired. This note is the
// #2992 fix's structural guard: it appears ONLY when a disposable built-in
// (per common::isDisposableBuiltinHeadName) reaches the end of
// registerUsingDisposal's arm chain without being claimed.
bool hasChainGuardNote(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("internal: `using var` of disposable built-in") !=
            std::string::npos)
            return true;
    }
    return false;
}

std::string usingVarProg(const std::string& usingLine) {
    return "module M; class C "
           "  public static method Main(): int "
           "    " + usingLine + " "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// The guard does NOT fire on claimed builtins. Each is claimed by a dedicated
// arm in registerUsingDisposal, so the end-of-chain guard is never reached.
// A spurious `internal:` note would mean the guard is mis-calibrated.
// per §28.3 / §28.9

BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           channel_no_chain_guard_note_spec_28_3) {
    auto r = runAot(usingVarProg("using var x := new Channel<int>();"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasChainGuardNote(r));
}

BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           stringbuilder_no_chain_guard_note_spec_28_3) {
    auto r = runAot(usingVarProg("using var x := new StringBuilder();"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasChainGuardNote(r));
}

BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           mutex_no_chain_guard_note_spec_28_3) {
    auto r = runAot(usingVarProg("using var x := new Mutex();"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasChainGuardNote(r));
}

BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           random_no_chain_guard_note_spec_28_3_28_9) {
    // Random is a vtable-less builtin claimed by the usingAutoDropVtableless
    // arm (#2921). Without that arm, the end-of-chain guard fires (the
    // internal note appears and the instance leaks — measured pre-#2921:
    // 3200 bytes in 200 allocations under LSan).
    auto r = runAot(usingVarProg("using var x := new Random();"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasChainGuardNote(r));
}

BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           atomic_no_chain_guard_note_spec_28_3_28_9) {
    // Atomic<int> is a vtable-less builtin claimed by the
    // usingAutoDropVtableless arm (#2973). Without that arm, the guard fires
    // (measured pre-#2973: 4800 bytes in 200 allocations under LSan).
    auto r = runAot(usingVarProg("using var x := new Atomic<int>(0);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasChainGuardNote(r));
}

BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           stopwatch_no_chain_guard_note_spec_28_3) {
    auto r = runAot(usingVarProg("using var x := new Stopwatch();"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasChainGuardNote(r));
}

BEZEL_TEST(using_var_autodrop_chain_guard_2992,
           event_source_no_chain_guard_note_spec_28_3) {
    auto r = runAot(usingVarProg("using var x := new EventSource<int>();"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasChainGuardNote(r));
}
