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
// discarded value's type is a KNOWN vtable-less builtin
// (`common::isVtableLessBuiltinClassName`) — the SAME authority the release
// dispatch two lines below already gates on, so the generic-class
// `emitDisposeAndReclaim` vtable dispatch (the wild jump #2675 exists to
// prevent) can never fire on the object recovered this way.
// `unwindOwnedLocalClass` itself is deliberately untouched: it also drives
// `registerUnwindOwnedLocal`'s EXCEPTION-unwind registration, which
// dispatches through the SAME vtable call — widening it would reintroduce
// the #2675 wild-jump crash on the unwind edge.
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
