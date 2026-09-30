// Spec-derived regression test for issue #4209.
//
// §28.2 (`IDisposable` / disposal exactly once) / §28.9 (Runtime-managed
// types) / §8.1 (`Object` is the universal root), v0.298.
// stdlib §1 ("Every class implicitly implements `IDisposable`"),
// stdlib §3.1 (`StringBuilder`), stdlib §6.2 (`Mutex`), stdlib §8.1
// (`Stopwatch`).
//
// THE DEFECT. `x?.Dispose()` (the null-conditional spelling) on a receiver
// whose static type is a VTABLE-LESS built-in class (StringBuilder, Mutex,
// Stopwatch, ...) wild-jumps: `lowerNullCondCall`'s `?.Dispose()` arm
// (`LlvmBackend_Calls2.cpp`) called `explicitDisposeOwnedReceiverSlot`
// UNCONDITIONALLY for every class-shaped receiver, then fell to
// `emitDynamicDispose` when it returned null — reading word 0 of the
// receiver as a Bezel vtable and indirect-calling slot 2. A built-in class
// carries no such vtable (word 0 is payload data), so this is a wild jump,
// the same #2921/#2967/#2973 family the non-`?.` `.Dispose()` arm was fixed
// for — but that fix's `isVtableLessBuiltinClassName` guard was never
// mirrored onto this `?.` arm.
//
// TWO DISTINCT SHAPES, BOTH FIXED HERE.
//   * A PLAIN `var` builtin: `unwindOwnedLocalClass` has no entry for a
//     built-in (it is scoped to user classes with a *generated* Dispose), so
//     `explicitDisposeOwnedReceiverSlot` returns null and the `else` arm's
//     `emitDynamicDispose(recvD)` wild-jumps. MEASURED on 3999278 (this
//     fix's parent, before the fix): `var sb := new StringBuilder("hi");
//     sb?.Dispose();` — `AddressSanitizer: SEGV on unknown address
//     0x000000000000 ... pc points to the zero page`, the exact repro filed
//     with #4209.
//   * A `using var` builtin: `explicitDisposeOwnedReceiverSlot`'s
//     `loc->isUsing` short-circuit returns a REAL slot regardless of
//     receiver type, so `emitDisposeAndReclaim` (itself `emitDynamicDispose`
//     + reclaim) wild-jumps the same way. #4209 flagged this as "not
//     investigated further" — MEASURED here to crash identically
//     pre-fix (same pc=0 SEGV signature).
//
// THE FIX intercepts a vtable-less-builtin receiver in the `?.Dispose()` arm
// BEFORE `explicitDisposeOwnedReceiverSlot` runs, routing it through the
// SAME `emitVtablelessBuiltinRelease` the non-`?.` arm already uses, gated
// on THAT function's own success (not merely on `isVtableLessBuiltinClassName`,
// which is also true of Array/Dictionary/Set/... — those have no arm in
// `emitVtablelessBuiltinRelease` and are NOT covered by this fix; see the
// scope note below and the accompanying lane report / follow-up issue).
//
// WHY THESE ORACLES. A zero exit code is not evidence of disposal (#2921's
// own history). So every crash-fix row asserts a DISTINCT non-zero exit
// value a SIGSEGV cannot produce, and every disposal/leak claim is a
// loop-amplified ASan/LSan run (`runAotChecked`, generated-code ASan) rather
// than a one-shot check.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody, const std::string& prelude = "") {
    return "module M; import System.Diagnostics; " + prelude +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// PLAIN `var` — the exact #4209 repro, plus two more vtable-less builtins to
// prove the fix is on the CLASS (the `isVtableLessBuiltinClassName` gate),
// not on a single name. Each SIGSEGV'd (exit 139 under a plain build, an
// ASan abort under `--asan`) pre-fix; the distinct return value cannot be
// produced by a crash.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_nullcond_dispose_vtableless_4209,
           stringbuilder_plain_var_nullcond_dispose_does_not_jump_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var sb := new StringBuilder(\"hi\"); sb?.Dispose(); "
                 "return 41;")),
        41);
}

BEZEL_TEST(codegen_nullcond_dispose_vtableless_4209,
           mutex_plain_var_nullcond_dispose_does_not_jump_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var m := new Mutex(); m?.Dispose(); return 42;")),
        42);
}

BEZEL_TEST(codegen_nullcond_dispose_vtableless_4209,
           stopwatch_plain_var_nullcond_dispose_does_not_jump_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var sw := new Stopwatch(); sw?.Dispose(); return 43;")),
        43);
}

// ---------------------------------------------------------------------------
// `using var` — the shape #4209 flagged as "not investigated further".
// `explicitDisposeOwnedReceiverSlot`'s `loc->isUsing` short-circuit hands
// back a real slot for ANY receiver type, so this reaches the SAME
// `emitDisposeAndReclaim` wild jump via the `if (slot)` branch instead of the
// `else` — a distinct crash path from the plain-`var` row above, confirmed
// broken pre-fix with the identical pc=0 SEGV signature.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_nullcond_dispose_vtableless_4209,
           stringbuilder_using_var_nullcond_dispose_does_not_jump_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("using var sb := new StringBuilder(\"hi\"); sb?.Dispose(); "
                 "return 44;")),
        44);
}

// ---------------------------------------------------------------------------
// §28.2 exactly-once: the explicit `?.Dispose()` call AND the `using var`
// scope-exit auto-drop both reach the same receiver in one scope. A fix that
// released without nulling the slot would turn the SIGSEGV above into a
// DOUBLE FREE here, which generated-code ASan reports. 200 iterations so a
// single-shot fluke cannot pass.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_nullcond_dispose_vtableless_4209,
           stringbuilder_nullcond_explicit_then_autodrop_exactly_once_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  using var sb := new StringBuilder(); "
            "  sb.Append(\"z\"); "
            "  sb?.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;")),
        0);
}

// A REPEATED explicit `?.Dispose()` on the same receiver must also be a
// no-op (§28.2: "the first call performs the disposal; subsequent calls are
// no-ops") — the second call reloads the now-null slot, so the `?.`
// null-guard itself skips it. A fix that nulled the slot incorrectly (or not
// at all) would double-free here.
BEZEL_TEST(codegen_nullcond_dispose_vtableless_4209,
           stringbuilder_nullcond_dispose_repeated_is_noop_28_2) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  var sb := new StringBuilder(); "
            "  sb.Append(\"z\"); "
            "  sb?.Dispose(); "
            "  sb?.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;")),
        0);
}

// ---------------------------------------------------------------------------
// §28.9 "the compiler frees the undischarged value at its owner's release
// point" — the under-release direction. LeakSanitizer over 200 iterations of
// the plain-`var` explicit-dispose shape.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_nullcond_dispose_vtableless_4209,
           stringbuilder_nullcond_dispose_plain_var_is_leak_free_28_9) {
    BEZEL_EXPECT_EQ(
        runChecked(prog(
            "var i := 0; "
            "while i < 200 do "
            "  var sb := new StringBuilder(); "
            "  sb.Append(\"z\"); "
            "  sb?.Dispose(); "
            "  i := i + 1; "
            "end; "
            "return 0;")),
        0);
}

// ---------------------------------------------------------------------------
// §7.5 null-receiver short-circuit control: a genuinely null receiver must
// remain a clean no-op (the `?.` guard itself, unaffected by this fix).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_nullcond_dispose_vtableless_4209,
           stringbuilder_nullcond_dispose_null_receiver_is_noop_7_5) {
    BEZEL_EXPECT_EQ(
        run(prog("var sb: StringBuilder? := null; sb?.Dispose(); "
                 "return 45;")),
        45);
}

// ---------------------------------------------------------------------------
// Guard: a USER class's `?.Dispose()` is unaffected — it has a real vtable
// (`isVtableLessBuiltinClassName` is false for it), so it still reaches the
// pre-existing `explicitDisposeOwnedReceiverSlot` / `emitDisposeAndReclaim`
// path below this fix's new branch, dispatching its destructor and
// reclaiming its block exactly once.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_nullcond_dispose_vtableless_4209,
           user_class_nullcond_dispose_unchanged_28_2) {
    BEZEL_EXPECT_EQ(
        run(prog("var u := new U(); u?.Dispose(); return U.Disposed;",
                 "class U public static Disposed: int := 0; "
                 "  public constructor() end constructor; "
                 "  destructor U.Disposed := U.Disposed + 1; end destructor; "
                 "end U; ")),
        1);
}
