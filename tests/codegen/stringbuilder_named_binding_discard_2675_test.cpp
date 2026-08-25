// Codegen — issue #2675, named-binding discard of a StringBuilder wild-calls
// (§17.13 announced consume, §28.3 unbound owned temporaries, §28.9
// trivial-dispose classes, §6.5 StringBuilder contract; spec v0.278).
//
// §17.13's "Discard of a named owned binding" reads: "`_ := x;` where `x` is a
// named owned binding — an owned local or an `owned` parameter — is a
// target-announced consume: the value is disposed at the end of the discard
// statement". For a `using var sb := new StringBuilder();` (an owned local) the
// discard `_ := sb;` is the same shape the existing class-arm codegen
// (LlvmBackend_Stmts.cpp's `discardOutOwnedVar` path) takes — but it issues
// `emitDisposeAndReclaim(obj)`, which reads the StringBuilder's word 0 as a
// vtable and indirect-calls slot 2. StringBuilder is a `bz_array_t*` with no
// Bezel vtable (the same fact every other vtable-less builtin reuses), so the
// indirect-call lands on random bytes and SIGSEGVs (measured pc = 0x0, exit
// 1, the same signature #3157/#2973 recorded for `Random` and Channel<T>).
//
// This is the named-binding twin of #2655 — which fixed the FRESH-producer
// arm (`_ := new StringBuilder();`) but left the named-binding arm on the
// class dispatch. Both shapes need the SAME routing (through
// `emitVtablelessBuiltinRelease`, which #1185 proved for `using var sb` and
// #2921/#2969/#2973 widened for `Random`/`Atomic<T>`/`Channel<T>`/the TCS
// family). Only StringBuilder is in the fix because the other built-in
// named-bindings (Random, EventSource, …) already reach a free path through
// `registerUsingDisposal`'s scope-exit registration, and `_ := sb;` is the
// observable breakage on this revision.
//
// SOLE-OWNER PROOF (why this cannot double-free): the named-binding discard
// is §17.13's announced consume — `sb` is left Disposed (K3.7's target state
// after `markDisposedByName`), the auto-drop registration (the §28.10
// `using var` scope-exit free) reloads null and no-ops. A borrow-init alias
// (a `using var alias := r;` borrow-pin) keeps `usingVarSourceIsBorrow` true
// and the `markDisposedByName`/`discardOutOwnedVar` path is skipped (§3253),
// so the borrow-pin's real owner keeps the obligation; this fix is restricted
// to the Genuine-Owner shape.
//
// EXACTLY-ONCE ORACLE: a trivial-dispose type has no destructor to count from
// (§28.9), so the sanitizer is the counter — under `runAotChecked` a wild
// free reads garbage and ASan raises DEADLYSIGNAL with pc=0 (a literal
// indirection to NULL), flipping `r.ran` to false → the -999 sentinel. A
// missing free is a LSan leak; a second free is an ASan double-free. Both
// flip the oracle.
//
// fail-without/pass-with (measured on 39c03b1 with `--sanitize=address`):
// `using var sb := new StringBuilder(); _ := sb;` ⇒ `DEADLYSIGNAL`,
// `ERROR: AddressSanitizer: SEGV on unknown address 0x000000000000
// (pc 0x000000000000 bp ... sp ... T0)` — the wild indirect-call signature.
// per §17.13, §28.3, §28.9 (issue #2675, spec v0.278)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
int runValueOnly(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The defect's primary repro (measured on 39c03b1): the §17.13 announced-
// consume of a named StringBuilder via a `using var`. The codegen reaches
// `discardOutOwnedVar` → `emitDisposeAndReclaim(obj)`, reads the builder's
// first word as a vtable, and indirect-jumps to NULL.
// Without the fix: SEGV, ASan DEADLYSIGNAL, r.ran=false → -999.
// Value: 0.
// per §17.13 announced consume, §28.9, §6.5 (issue #2675)
BEZEL_TEST(stringbuilder_named_binding_discard_2675, using_var_discarded_via_underscore_announce) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    _ := sb; "
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}

// ── The plain-`var` twin: the auto-drop scope-end path ALREADY routes through
// `emitVtablelessBuiltinRelease` for StringBuilder (#1185), so the discard
// here is a confirmed no-op (the slot is nulled, the auto-drop reloads null
// and skips). This test pins that the named-binding discard is exactly-once
// across the plain-`var` AND `using var` shapes. Value: 0.
// per §17.13 announced consume, §28.3 (issue #2675)
BEZEL_TEST(stringbuilder_named_binding_discard_2675, plain_var_discarded_via_underscore_announce) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var sb := new StringBuilder(); "
        "    _ := sb; "
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}

// ── With PARTS to reclaim: the parts array carries the +1 string refs;
// the free MUST release those as well. `using var sb := new StringBuilder("ab");`
// then append → discard must free the array AND the retained strings. Value: 0.
// per §6.5 StringBuilder contract, §17.13 (issue #2675)
BEZEL_TEST(stringbuilder_named_binding_discard_2675, using_var_with_retained_parts_discarded) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(\"ab\"); "
        "    _ := sb.Append(\"cd\"); "
        "    _ := sb.AppendLine(\"ef\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        0);
}

// ── LOUD form: the discard inside a loop, so a missing free is 8 leaked
// builders, not 1. The free must fire per ITERATION. Value: 8.
// per §17.13, §28.3 (issue #2675)
BEZEL_TEST(stringbuilder_named_binding_discard_2675, using_var_discarded_in_loop_frees_per_iteration) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..8 do "   // §17.9: `a..b` is INCLUSIVE — 8 iterations
        "      using var sb := new StringBuilder(\"ab\"); "
        "      _ := sb; "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;"),
        8);
}

// ── REGRESSION GUARD for the untouched neighbour: a `using var` builder that
// is used normally (no discard). Its #1185 scope-end free must remain the ONLY
// one — this program contains no discard at all, so any change in its
// behaviour would mean the new arm fired somewhere it must not. Value: 4.
// per stdlib §3.1, §28.10 (issue #2675 self-pass)
BEZEL_TEST(stringbuilder_named_binding_discard_2675, using_var_builder_unchanged_stdlib_3_1) {
    const std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    sb.Append(\"ab\"); "
        "    sb.Append(\"cd\"); "
        "    return sb.ToString().Length; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 4);
    BEZEL_EXPECT_EQ(runValueOnly(prog), 4);
}

// ── NO-DOUBLE-FREE PRE-EMPT: the scope-exit auto-drop is the SECOND free, so
// after the discard nulls the slot, the using-var's `registerUsingDisposal`
// registered disposer reloads null and skips. Without the null-after-discard
// rule, this would be a double-free — exactly the §2655 defect family.
// Value: 0; the program runs the discard + the using-var's auto-drop in
// sequence and must not crash.
// per §28.10, §17.13 (issue #2675 self-pass)
BEZEL_TEST(stringbuilder_named_binding_discard_2675, using_var_discarded_auto_drop_is_null_safe) {
    const std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(\"hello\"); "
        "    _ := sb; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 0);
    BEZEL_EXPECT_EQ(runValueOnly(prog), 0);
}
