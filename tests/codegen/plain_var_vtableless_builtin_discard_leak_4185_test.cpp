// Codegen — issue #4185: a plain `var` (non-`using`) named discard of a
// vtable-less builtin leaks the instance (§17.13 "Discard of a named owned
// binding", §28.9 trivial-dispose classes; spec v0.298).
//
// §17.13's "Discard of a named owned binding" paragraph: "`_ := x;` where `x`
// is a named owned binding — an owned local or an `owned` parameter — is a
// marker-free consume: the value is disposed at the end of the discard
// statement". The same section's owned-result paragraph specializes this for
// a *trivial-dispose* class ([§28.9](#sec-runtime-managed-types)): "the value
// is **freed** at the end of the discard statement, and the free runs no user
// code" — the spec's own worked example is `_ := new Random(); // trivial-
// dispose (§28.9): freed at end of statement; no user code runs". StringBuilder
// and Mutex are trivial-dispose classes in exactly this sense (no user
// destructor; disposal releases only compiler-managed memory, stdlib §6.5 /
// §6.2), so `var sb := new StringBuilder(); _ := sb;` must free `sb` by the
// same rule the spec's fresh-temporary example already pins.
//
// ROOT CAUSE (differs from the issue's own framing): the issue's title reads
// "discarding two DIFFERENT ... locals ... leaks one of them", suggesting the
// defect is about a TWO-LOCAL interaction. Tracing the codegen
// (LlvmBackend_Stmts.cpp's `discardOutOwnedVar` path) shows the actual defect
// is unconditional: the named-binding discard's vtable-less release arm
// resolves the local's slot via `explicitDisposeOwnedReceiverSlot`, whose
// LocalSymbol case only recognizes a slot as "this scope must reclaim it" via
// `loc->isUsing` (a `using var`) or `unwindOwnedLocalClass(ld)` — which is
// scoped to a USER class/interface with a *generated* Dispose
// (`disposerFns_`) and returns null for every vtable-less BUILTIN (they have
// no such generated function). So a PLAIN `var` discard of ANY vtable-less
// builtin (StringBuilder, Mutex, Stopwatch, ...) finds no slot and the whole
// branch emits NOTHING — a leak — independent of whether a second, different
// local is discarded nearby. The issue's own "two different types" framing
// is an artifact of LeakSanitizer's one-shot conservative stack scan: a
// SOLO `var sb := new StringBuilder(); _ := sb; return 0;` measured 0 bytes
// leaked under one-shot LSan (the dead pointer is still conservatively
// "reachable" from a stale stack slot at exit) while looping the identical
// shape 200x, or pairing it with enough intervening codegen (a second local's
// initialization, as the issue's repro does), reliably surfaces the leak by
// overwriting that stale slot before exit. See LANE_RULES.md §3 / the
// project's "one-shot LSan passes vacuously" precedent.
//
// FIX: LlvmBackend_Stmts.cpp's DiscardStmt vtable-less/class branch now
// falls back to `lvalueAddr` (bypassing `unwindOwnedLocalClass`, NOT
// widening it) when `explicitDisposeOwnedReceiverSlot` finds no slot AND the
// discarded value's type is one `emitVtablelessBuiltinRelease` actually
// KNOWS HOW TO RELEASE (`Emitter::vtablelessBuiltinReleaseIsKnown`), so the
// generic-class `emitDisposeAndReclaim` vtable dispatch (the wild jump #2675
// exists to prevent) can never fire on the object recovered this way.
// `unwindOwnedLocalClass` itself is deliberately untouched: it also drives
// `registerUnwindOwnedLocal`'s EXCEPTION-unwind registration, which
// dispatches through the SAME vtable call — widening it would reintroduce
// the #2675 wild-jump crash on the unwind edge.
//
// fix-forward round 1 (review NEEDS-FIX, §25): the FIRST version of this fix
// gated the fallback on `common::isVtableLessBuiltinClassName` alone — a
// predicate validated to answer "carries no Bezel vtable" (47 names), a
// STRICT SUPERSET of what `emitVtablelessBuiltinRelease` actually handles
// (18 names). For a name in the 29-name gap (`Shared<T>`, `Weak<T>`,
// `JsonSerializer`, `BinarySerializer`, `JsonDeserializer`,
// `BinaryDeserializer`, `SharedEventSource<T>`, `Buffer`, `TaskScope`, ...)
// that version's fallback found a slot, `emitVtablelessBuiltinRelease`
// returned false, and the pre-existing `else` branch called
// `emitDisposeAndReclaim` on it — the SAME #2675 wild jump, converting a
// leak into a crash. Measured (reviewer's independent build) on
// `var sh := new Shared<Widget>(new Widget()); _ := sh;`: a
// **heap-buffer-overflow**, not a SEGV — the wild read landed inside mapped
// memory, so ASan's redzone check fired first (see the control row below
// for the "wild jump is a crash FAMILY, not one signature" note).
//
// fix-forward round 1's remedy was `vtablelessBuiltinReleaseIsKnown`, a
// second, small, hand-synced 18-name array — justified at the time as
// unavoidable because `emitVtablelessBuiltinRelease` cannot be asked
// "would you handle this" without emitting IR on every success arm. Review
// round 2 found the actual unavoidable constraint narrower than that: only
// the EMISSION half of that function needs IR access, not the NAME-MATCHING
// half. `emitVtablelessBuiltinRelease` (LlvmBackend_Disposal.cpp) is now
// split into a pure `classifyVtablelessBuiltinRelease` (bn -> which arm, if
// any — zero IR side effects) and an emission switch that consumes its
// result; `vtablelessBuiltinReleaseIsKnown` also calls the classifier
// instead of carrying its own list. One name-matching site, not two — the
// hand-synced array from round 1 is gone entirely, closing the "no ratchet
// covers table drift" gap the round-2 review flagged (§6).
//
// Either round's fix leaves a name in the 29-name gap with the fallback's
// slot null and the branch emitting nothing for it — a leak, matching
// PRE-#4185 behaviour for that untested tail, never a crash.
// `shared_uncovered_builtin_still_leaks_not_crashes` below is the control
// row round 1's review asked for.
//
// EXACTLY-ONCE ORACLE: `runAotChecked` compiles the generated code with
// AddressSanitizer and runs it under LeakSanitizer (detect_leaks=1); a leak,
// double-free, or use-after-free all surface as `ran=false` with the report
// in `diagnostics`, flipping the `runChecked` wrapper below to -999. A
// missing free is an LSan leak; a wrongly-added double free would be an
// ASan double-free — both flip the same oracle.
//
// FAIL-WITHOUT/PASS-WITH (measured on 3999278 + this fix, `--sanitize=address`):
// all four loop-amplified rows below FAIL (LeakSanitizer report, r.ran=false,
// runChecked() == -999) on the pre-fix tree and PASS (== 200) post-fix; see
// the commit body for the exact counts. // per §17.13, §28.9, v0.298 (issue #4185)

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

// ── PRIMARY: a SOLO plain-`var` StringBuilder discarded 200x in a loop.
// Each iteration is byte-identical to the issue's own repro 1's first
// statement, with no second local at all — pins that the defect is NOT
// about two different types, only about the plain-`var` vtable-less-builtin
// shape. Without the fix: LSan reports the accumulated leak (568 B x up to
// 200, minus whatever the conservative stack scan still finds reachable) and
// `r.ran` flips false -> -999. With the fix: 200.
// per §17.13 (named-binding discard), §28.9 (trivial-dispose), stdlib §6.5
// (issue #4185)
BEZEL_TEST(plain_var_vtableless_builtin_discard_leak_4185, solo_stringbuilder_looped_200) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..200 do "
        "      var sb := new StringBuilder(\"x\"); "
        "      _ := sb; "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;"),
        200);
}

// ── The Mutex twin: pins the fix is general across the vtable-less-builtin
// family (`common::isVtableLessBuiltinClassName`), not StringBuilder-specific.
// per §17.13, §28.9, stdlib §6.2 (issue #4185)
BEZEL_TEST(plain_var_vtableless_builtin_discard_leak_4185, solo_mutex_looped_200) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..200 do "
        "      var m := new Mutex(); "
        "      _ := m; "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;"),
        200);
}

// ── The issue's OWN repro shape, looped: a plain-`var` StringBuilder
// discarded immediately before a plain-`var` Array<int> discarded in the
// same iteration — pins the exact reported symptom (pairing two different
// vtable-less/collection locals) in addition to the solo rows above.
// per §17.13, §28.5, §28.9, stdlib §6.5, §4.3 (issue #4185)
BEZEL_TEST(plain_var_vtableless_builtin_discard_leak_4185, stringbuilder_then_array_looped_200) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..200 do "
        "      var y := new StringBuilder(); "
        "      _ := y; "
        "      var arr := new Array<int>(3, 0); "
        "      _ := arr; "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;"),
        200);
}

// ── NO-DOUBLE-FREE PRE-EMPT / REGRESSION GUARD: a `using var` builder and a
// plain `var` builder discarded together in the same iteration. The
// `using var` spelling already routed through `emitVtablelessBuiltinRelease`
// before this fix (#2675) — this pins that recovering the plain-`var`
// slot via the new `lvalueAddr` fallback does not disturb the `using var`
// arm (`loc->isUsing` still short-circuits first) and that neither slot is
// freed twice. Without the fix this row would ALSO fail (the plain-`var`
// leg leaks), so it does not by itself distinguish the two defects, but a
// regression that double-frees either leg would show as an ASan report
// here, distinctly from a pure leak.
// per §17.13, §28.9, §28.10, stdlib §6.5 (issue #4185)
BEZEL_TEST(plain_var_vtableless_builtin_discard_leak_4185, using_and_plain_var_mixed_looped_200) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..200 do "
        "      using var u := new StringBuilder(\"u\"); "
        "      _ := u; "
        "      var p := new StringBuilder(\"p\"); "
        "      _ := p; "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;"),
        200);
}

// ── CONTROL — an UNCOVERED vtable-less builtin (`Shared<T>`; NOT one of the
// 18 names `emitVtablelessBuiltinRelease` handles) discarded via a plain
// `var` must still LEAK CLEANLY, never crash, through this path. This is
// the row the NEEDS-FIX review asked for (R17 #4196's lesson: a narrowing
// is unproven without a control from the excluded set). The FIRST version
// of this fix gated its fallback on `common::isVtableLessBuiltinClassName`
// alone (proves only "no vtable", true for `Shared<T>` too) and this exact
// program crashed under `--sanitize=address` — the reviewer's independent
// build reproduced a **heap-buffer-overflow**, not a SEGV: the wild
// `emitDisposeAndReclaim` read of word 0 / slot 2 landed inside a mapped
// (allocated) region rather than an unmapped page, so ASan's own
// out-of-bounds check fires before any illegal instruction fetch would —
// a reminder that "wild jump" is a FAMILY of crash shapes (SEGV,
// heap-buffer-overflow, occasionally a clean abort), not one fixed
// signature, because it depends on what happens to be mapped at the bogus
// address `emitDisposeAndReclaim` computes. `emitVtablelessBuiltinRelease`
// returned false for "Shared" and the pre-existing `else` branch dispatched
// `emitDisposeAndReclaim` on the newly-recovered slot regardless.
// `vtablelessBuiltinReleaseIsKnown` narrows the gate to exactly the covered
// set, so `Shared<T>` now leaves the fallback's slot null and the branch
// emits nothing — a leak (the pre-#4185, still-safe outcome for this
// untested tail), asserted below by a LeakSanitizer report with NO
// SEGV / heap-buffer-overflow / wild-jump / DEADLYSIGNAL text anywhere in
// the diagnostics. Looped 200x for the same one-shot-LSan reason the rows
// above are.
// per §25, §28.13 "Shared ownership" (`Shared<T>`), issue #4185
BEZEL_TEST(plain_var_vtableless_builtin_discard_leak_4185, shared_uncovered_builtin_still_leaks_not_crashes) {
    const std::string prog =
        "module M; "
        "class Widget end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..200 do "
        "      var sh := new Shared<Widget>(new Widget()); "
        "      _ := sh; "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;";
    // The plain (non-ASan) build must not crash either way — a leak is
    // invisible to it, a wild jump would not be.
    BEZEL_EXPECT_EQ(runValueOnly(prog), 200);
    // Under ASan/LSan the accumulated leak IS caught (ran flips false) —
    // that is the EXPECTED outcome for a name outside
    // `emitVtablelessBuiltinRelease`'s coverage. The load-bearing check is
    // the SHAPE of the failure: a LeakSanitizer report, never any of the
    // "wild jump" crash family's known signatures — SEGV, DEADLYSIGNAL, or
    // (the reviewer's own reproduction of THIS exact program, pre-fix)
    // heap-buffer-overflow: the bogus `emitDisposeAndReclaim` read can land
    // inside mapped memory rather than an unmapped page, so ASan's redzone
    // check fires before any illegal fetch would.
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    bool sawLeakReport = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_EQ(d.message.find("SEGV"), std::string::npos);
        BEZEL_EXPECT_EQ(d.message.find("DEADLYSIGNAL"), std::string::npos);
        BEZEL_EXPECT_EQ(d.message.find("wild jump"), std::string::npos);
        BEZEL_EXPECT_EQ(d.message.find("heap-buffer-overflow"),
                        std::string::npos);
        if (d.message.find("LeakSanitizer") != std::string::npos ||
            d.message.find("detected memory leaks") != std::string::npos)
            sawLeakReport = true;
    }
    BEZEL_EXPECT_TRUE(sawLeakReport);
}

// ── VALUE-ONLY SANITY (no ASan): the primary row must also compile and run
// correctly on the plain (non-sanitized) build — a leak fix must not change
// the observable VALUE the program computes.
// per §17.13 (issue #4185 self-pass)
BEZEL_TEST(plain_var_vtableless_builtin_discard_leak_4185, solo_stringbuilder_value_only) {
    const std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var sb := new StringBuilder(\"ab\"); "
        "    _ := sb; "
        "    return 7; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runValueOnly(prog), 7);
    BEZEL_EXPECT_EQ(runChecked(prog), 7);
}
