// Spec-derived tests for issue #2629 — an inline `out var` recipient of
// NON-NULLABLE REFERENCE type had its caller-side slot left un-stored, so the
// CALLEE's own assignment to the `out` parameter released raw stack garbage.
//
// WHAT THE DEFECT WAS. `ensureOutVarSlots` (method/function calls) and
// `ensureCtorOutVarSlots` (`new T(...)`) — both in
// `src/codegen/LlvmBackend_Calls2.cpp` — allocate the caller-side slot for an
// inline `out var` and zero-store it, but the zero-store was CONDITIONAL:
// skipped exactly when the recipient's type `isNonNullableReferenceType`. That
// exemption was introduced with #2395 / #2402 and is documented at both sites;
// its stated ground is that §22.4's "Scope is not definite assignment" makes a
// read of such a recipient on a call-skipped path a COMPILE error, so no §7.9
// default is required — "storing one would just be dead weight".
//
// That reasoning considers only what the CALLER may read back. It misses the
// second consumer of the slot's incoming bits: the CALLEE. §22.3 "Parameter
// modifiers" (Bezel_Language_Spec.md v0.236) says of `out`: "within the callee
// the parameter denotes a callee-local slot" and "The variable need not be
// initialized before the call" — but the same bullet blesses the shortcut this
// backend takes, "An implementation may pass a hidden reference". Passing the
// caller's slot by hidden reference means the callee's `n := "hi"` lowers to
// the ordinary assign sequence, which for a REFCOUNTED referent (string) first
// LOADS the slot's prior value and calls `bz_string_release` on it. With the
// slot un-stored that prior value is raw stack garbage, so the callee performs
// a wild release — `bz_string_release` dereferences it to read the refcount
// word (`bezel_runtime_refcounting.cpp:125-131`) and the process SIGSEGVs.
//
// So the un-stored slot is not merely "no default promised" — it is a live
// memory-safety defect on the ALWAYS-TAKEN path, with no short-circuit or `?.`
// skipping involved at all. Pre-fix, `new Widget("x", out var n)` with
// `out n: string` SIGSEGVs (exit 139) on a straight-line call.
//
// THE FIX makes the zero-store UNCONDITIONAL at both sites. It cannot make an
// accepted program observe the stored `null`: §22.4's DA rule still rejects any
// read of a non-nullable-reference out variable on a maybe-skipped path, so the
// null exists solely to give the callee's release-of-prior a defined operand,
// and `bz_string_release(NULL)` is a documented no-op (same file, line 126).
//
// ⚠ ADDRESSSANITIZER MASKS THIS DEFECT — do not use it as the oracle here.
// Under `BEZEL_AOT_FORCE_SANITIZE=address` the same three pre-existing RED
// tests in `tests/sema/ctor_out_var_nullable_da_2409_test.cpp` all PASS, because
// ASan's stack instrumentation does not leave the reused slot holding the
// non-null garbage the plain build does. Every test in this file was verified
// red-without / green-with the fix on the PLAIN (non-sanitized) build. If a
// future change regresses this, the ASan gate will stay green and only the
// plain path will show it.
//
// WHY THE METHOD-CALL CASE NEEDS ITS OWN TEST. The ctor half is already pinned
// by the three tests in `tests/sema/ctor_out_var_nullable_da_2409_test.cpp`
// (`plain_call_nonnullable_reference_read_accepted` and siblings), which were
// RED on main. The METHOD half (`ensureOutVarSlots`) carries the identical
// defective predicate but no test observed it, because in a small `Main` the
// reused stack slot happens to already be zero — the wild release is a no-op by
// luck. `method_out_var_reference_after_prior_call_returns_real_value` below
// forces the issue by making a prior call first, so the slot the `out var`
// alloca lands on holds that frame's non-null residue. Mechanically verified:
// with the fix reverted that test SIGSEGVs; with the fix it returns 2.
//
// ⚠ That one test's redness is therefore stack-layout dependent (it needs the
// preceding frame to leave a non-null value in the reused slot). It is
// deterministic for a given compiler/codegen — re-run 10x pre-fix, SIGSEGV
// every time — but a future codegen change could in principle make it pass
// vacuously. The other tests here are ordinary value oracles and do not depend
// on stack residue.

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// -----------------------------------------------------------------------------
// The METHOD-call half of the defect (`ensureOutVarSlots`) — the half no
// existing test covered. `Dirty` runs first purely to leave non-null residue in
// the stack region `Main`'s `out var` slot is allocated from; without it the
// wild release happens to free nothing and the bug hides. Asserts the real
// VALUE (2 == "hi".Length), not merely "did not crash".
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_slot_release_2629,
           method_out_var_reference_after_prior_call_returns_real_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public constructor() end; "
        "  public static method Dirty(a: int, b: int, c: int, d: int): int "
        "    var p := new Widget(); "
        "    _ := p; "
        "    return a + b + c + d; "
        "  end Dirty; "
        "  public static method Make(s: string, out n: string): bool "
        "    n := \"hi\"; return true; "
        "  end Make; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var junk := Widget.Dirty(1, 2, 3, 4); "
        "    _ := junk; "
        "    var b := Widget.Make(\"x\", out var n); "
        "    _ := b; "
        "    return n.Length; "
        "  end Main; end C;"), 2);
}

// -----------------------------------------------------------------------------
// The CONSTRUCTOR half (`ensureCtorOutVarSlots`) — issue #2629's own repro,
// a straight-line `new` with no short-circuiting. This one needs no stack
// priming: `new`'s own `calloc` call leaves the residue. Pre-fix: exit 139.
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_slot_release_2629,
           ctor_out_var_reference_plain_call_returns_real_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := \"hi\"; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    return n.Length; "
        "  end Main; end C;"), 2);
}

// -----------------------------------------------------------------------------
// GENERIC class constructor — `ensureCtorOutVarSlots` reaches the same
// predicate through the reified-generic path, and it SIGSEGVs pre-fix
// identically. Nothing in the existing #2409 suite covers a generic ctor.
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_slot_release_2629,
           generic_ctor_out_var_reference_returns_real_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Box<T> "
        "  public ok: bool; "
        "  public constructor(seed: T, out n: string) "
        "    n := \"hi\"; ok := true; "
        "  end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var b := new Box<int>(5, out var n); "
        "    _ := b; "
        "    return n.Length; "
        "  end Main; end C;"), 2);
}

// -----------------------------------------------------------------------------
// CONTROL — a PRIMITIVE ctor out-var already took the zero-store branch before
// this fix and must be unchanged by making it unconditional. Passes identically
// pre- and post-fix (verified); it is here to catch an over-widening that broke
// the §7.9 default path the #2395/#2402 fixes established.
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_slot_release_2629,
           ctor_out_var_primitive_control_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: int) "
        "    n := 2; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(\"x\", out var n); "
        "    _ := w; "
        "    return n; "
        "  end Main; end C;"), 2);
}

// -----------------------------------------------------------------------------
// LOOP — the shape that dictated WHERE the fix's store goes. The recipient's
// slot is reused across iterations, and iteration N's callee release-of-prior
// is what reclaims iteration N-1's string. An initializing store emitted at the
// CALL SITE would run every iteration and clobber that pointer first, stranding
// one string per iteration; the fix therefore emits this shape's store ONCE, in
// the entry block. Measured under LSan on this exact program (5 iterations,
// heap strings via `s + "-tail"`):
//
//     pre-fix (no store at all)       39 B in 2 allocations
//     call-site store (rejected)     195 B in 10 allocations   <-- regression
//     entry-block store (shipped)     39 B in 2 allocations    <-- unchanged
//
// The residual 39 B is a PRE-EXISTING leak of the FINAL iteration's value at
// scope exit — a plain `out var` string recipient is never released when its
// block ends. It reproduces identically with this fix reverted, so it is not
// this fix's doing; filed separately. That is why this test asserts the VALUE
// (5 * len("x-tail") == 30) rather than leak-freedom: a leak assertion here
// would fail on that unrelated pre-existing defect and could not be made green.
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_slot_release_2629,
           loop_reused_out_var_slot_returns_real_value_each_iteration) {
    BEZEL_EXPECT_EQ(run(
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
// CONTROL — a PRE-DECLARED (non-inline) `out n` recipient never went through
// either ensure*OutVarSlots function; its slot carries the initializer's real
// value and the callee's release-of-prior was always correct. Guards against a
// fix that accidentally null-stored over a live reference (which would have
// leaked the initializer's string, or worse released it twice).
// -----------------------------------------------------------------------------
BEZEL_TEST(out_var_slot_release_2629,
           predeclared_out_recipient_control_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Widget "
        "  public ok: bool; "
        "  public constructor(s: string, out n: string) "
        "    n := \"hi\"; ok := true; "
        "  end; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var n: string := \"zz\"; "
        "    var w := new Widget(\"x\", out n); "
        "    _ := w; "
        "    return n.Length; "
        "  end Main; end C;"), 2);
}
