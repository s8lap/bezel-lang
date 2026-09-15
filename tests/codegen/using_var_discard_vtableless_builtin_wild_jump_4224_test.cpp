// Codegen — issue #4224: `using var` discard of an UNCOVERED vtable-less
// builtin wild-jumps via DiscardStmt's pre-existing `else` branch (§17.13
// "Discard of a named owned binding", §28.9 trivial-dispose classes / runtime
// -managed types, §28.3 producers / owned-local discharge; spec v0.298).
//
// THE DEFECT. `using var sh := new Shared<Widget>(new Widget()); _ := sh;`
// crashes with an AddressSanitizer heap-buffer-overflow. `_ := sh;` is §17.13's
// "discard of a named owned binding": `explicitDisposeOwnedReceiverSlot`
// (LlvmBackend_Calls2.cpp) resolves `sh`'s slot via its LocalSymbol arm, whose
// `if (loc->isUsing) return lvalueAddr(receiver, slotTy);` short-circuits
// straight past `unwindOwnedLocalClass` — it hands back a REAL slot for a
// `using var` binding of ANY type. DiscardStmt (LlvmBackend_Stmts.cpp) then
// dispatches on that slot: `common::isVtableLessBuiltinClassName("Shared")` is
// true, but `emitVtablelessBuiltinRelease("Shared", obj)` names no arm (only
// 18 of the 47 vtable-less-builtin-class names are covered), so the `&&`
// fails and the pre-existing `else` branch — untouched by #4185/#4209 — runs
// `emitDisposeAndReclaim(obj)` unconditionally: `Shared<T>`'s runtime
// representation (`bz_shared_t`, a plain 40-byte C struct — see
// `src/runtime/bezel_runtime_shared_weak.cpp`) is read as a Bezel vtable at
// word 0 and indirect-called at slot 2 — the same #2675/#4185/#4209 wild-jump
// family, one level up.
//
// MEASURED on 6f46a73 (this run's base, which already carries #4185's final
// classify/emit-split form and #4209): `scripts/agent_check.sh --probe --asan`
// on the exact program above —
//   ==ERROR: AddressSanitizer: heap-buffer-overflow ... READ of size 8 ...
//   0x... is located 0 bytes after 16-byte region [...]
//       #1 ... in calloc ... (bz_shared_new's allocation)
//   SUMMARY: AddressSanitizer: heap-buffer-overflow ... in main
// — the same pc offset (+0x4ea61a) and 16-byte allocation the issue's own
// filing cites.
//
// WHY THIS DIFFERS FROM #4185's OWN `Shared<T>` CONTROL ROW. #4185's
// `plain_var_vtableless_builtin_discard_leak_4185_test.cpp` already has a
// `shared_uncovered_builtin_still_leaks_not_crashes` row for `Shared<T>` — but
// its program uses a PLAIN `var sh := ...`, not `using var sh := ...`. For a
// plain `var`, `explicitDisposeOwnedReceiverSlot`'s LocalSymbol arm has NO
// `isUsing` short-circuit to take; it falls to `unwindOwnedLocalClass(ld)`
// (null for every builtin), then to #4185's own fallback gate
// (`vtablelessBuiltinReleaseIsKnown`, narrowed in #4185's OWN fix-forward),
// which correctly leaves `slotAddr` null for `Shared` and this whole branch
// emits nothing. The `using var` spelling never reaches that fallback at all
// — `explicitDisposeOwnedReceiverSlot` already returned non-null via the
// `isUsing` short-circuit, so the `else` branch's unconditional
// `emitDisposeAndReclaim` was the ONLY gate ever consulted for it. This is
// exactly the gap #4185's own fix-forward commit body flagged as out of
// scope ("Confirmed unchanged and out of scope per review: ... the `using
// var` short-circuit").
//
// THE FIX (LlvmBackend_Stmts.cpp, the `else` arm of the `discardOutOwnedVar`
// class/vtable-less dispatch): compute `isVtablelessBuiltin =
// !bn.empty() && common::isVtableLessBuiltinClassName(bn)` — the SAME
// authority the release-arm condition already consults, no new predicate —
// BEFORE deciding whether the `else` arm may call `emitDisposeAndReclaim`.
// Three-way split:
//   * `isVtablelessBuiltin && emitVtablelessBuiltinRelease(...)` succeeds →
//     release path, unchanged.
//   * `!isVtablelessBuiltin` (a genuine user class/interface, OR a reserved
//     BUILTIN INTERFACE head — `isVtableLessBuiltinClassName` excludes those
//     by its own contract because they keep real vtable dispatch) →
//     `emitDisposeAndReclaim` is safe, unchanged.
//   * `isVtablelessBuiltin` but the release call failed (the gap: `Shared<T>`,
//     `Weak<T>`, `JsonSerializer`, `BinarySerializer`, `SharedEventSource<T>`,
//     the `Buffer` family, ...) → leak instead of wild-jump (the SAME
//     leak-before-wild-jump trade #2973/#4185 make for this identical gap on
//     the plain-`var` spelling): null the slot so a following scope-end/
//     unwind auto-drop reloads null and no-ops, but do NOT call
//     `emitDisposeAndReclaim`.
//
// COLLECTIONS ARE OUT OF SCOPE, DELIBERATELY, AND STILL CRASH ELSEWHERE.
// `discardOutOwnedVar`'s COLLECTION branch (a few lines above the one this
// fix touches — `collLd` via `collectionTypeOfLocal`/`dictTypeOfLocal`/
// `linkedListTypeOfLocal`) claims OwningArray/OwningDictionary/OwningSet/
// OwningStack/OwningQueue/OwningLinkedList FIRST, for both `using var` and
// plain `var` — so no collection ever reaches the branch this fix changes,
// and this fix does not touch, mask, or otherwise affect them. The
// collection gap on the UNRELATED explicit `?.Dispose()` path (a different
// function, `lowerNullCondCall` in LlvmBackend_Calls2.cpp) is #4221, open,
// filed by the #4209 lane, which measured collections crashing identically
// on that path before and after #4209 — this fix leaves that exactly as it
// was.
//
// WHY THESE ORACLES. A zero/non-crash exit is not evidence of correct
// disposal (#2921's own history) — every crash-fix row below asserts the
// ABSENCE of every known wild-jump crash-signature string (SEGV,
// DEADLYSIGNAL, heap-buffer-overflow, "wild jump") rather than merely a
// non-crash exit, and every leak claim is loop-amplified (200x) under
// generated-code ASan/LSan (`runAotChecked`) rather than a one-shot check —
// LANE_RULES.md §3's "one-shot LSan passes vacuously" precedent (confirmed
// again while preparing this test: `Weak<Widget>`'s single-iteration leak of
// its shared `bz_shared_t` control block was invisible to LSan one-shot and
// only surfaced looped 300x, at 40 bytes x 299 objects).
//
// FAIL-WITHOUT/PASS-WITH (measured on 6f46a73 + this fix,
// `scripts/agent_check.sh --probe --asan`, single-iteration, before writing
// the loop-amplified rows below): `Shared<Widget>` — pre-fix
// heap-buffer-overflow (quoted above); post-fix a clean 40-byte LeakSanitizer
// report, `bz_shared_new` as the allocation site. `JsonSerializer` — post-fix
// a clean 136-byte report (`bz_jser_alloc`). `SharedEventSource<int>` —
// post-fix a clean 32-byte report (`bz_shared_event_source_new`).
// `StringBuilder` (covered-set regression guard) — exit 0, no leak, both
// before and after (this fix does not touch that arm's condition).
// The gtest rows below reproduce all of these through the harness, looped,
// with the FAIL-WITHOUT side proven mechanically in the commit body.
// per §17.13, §28.3, §28.9, stdlib §2.16 (`Shared`/`Weak`), stdlib §2.15
// (`JsonSerializer`), stdlib §6.5 (`SharedEventSource`), stdlib §3.1
// (`StringBuilder`), stdlib §6.2 (`Mutex`) — issue #4224, v0.298

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int runValueOnly(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Assert the ASan/LSan run reports a leak (not a crash): `r.ran` is false
// (a report of ANY kind flips it), no crash-signature text appears anywhere
// in the diagnostics, and a LeakSanitizer report is present. Mirrors the
// #4185 control row's oracle exactly (LANE_RULES.md §18: a crash can carry a
// real source location too, so the text check — not merely `ran`/location —
// is what proves "leak, not crash").
void expectLeakNotCrash(const std::string& prog) {
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

// Assert the ASan/LSan run is CLEAN (the covered-release-arm regression
// guards): no report of any kind, and the program's return value is `n`.
void expectCleanRelease(const std::string& prog, int n) {
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, n);
}
}  // namespace

// ── PRIMARY (the issue's own repro): `using var sh := new
// Shared<Widget>(new Widget()); _ := sh;`, looped 200x. Without the fix: an
// AddressSanitizer heap-buffer-overflow (the exact signature quoted above),
// deterministic even at N=1 (a genuine memory-safety violation, not a
// one-shot-LSan-invisible leak) — so this row alone is the FAIL-WITHOUT
// proof for the crash-vs-leak split; looping is for the LEAK side (post-fix)
// to be observable at all, not for the crash side to reproduce.
// per §17.13, §28.9, §28.13 (issue #4224)
BEZEL_TEST(using_var_discard_vtableless_builtin_4224,
           shared_using_var_discard_looped_leaks_not_crashes) {
    const std::string prog =
        "module M; "
        "class Widget end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..200 do "
        "      using var sh := new Shared<Widget>(new Widget()); "
        "      _ := sh; "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;";
    // The plain (non-ASan) build must not crash either way, and must compute
    // the same VALUE a leak fix is not permitted to change.
    BEZEL_EXPECT_EQ(runValueOnly(prog), 200);
    expectLeakNotCrash(prog);
}

// ── `Weak<T>` (`sh.Downgrade()`): `Shared<T>` and `Weak<T>` share ONE
// runtime control block (`bz_shared_t`, `src/runtime/bezel_runtime_shared_
// weak.cpp`) with independent strong/weak refcounts; `Downgrade()` mints an
// alias of the SAME allocation rather than a new one. `sh` (the strong
// handle) is disposed normally at its own `using var` scope end (a DIFFERENT,
// unaffected code path — `registerUsingDisposal`'s dedicated Shared/Weak-
// aware arm, LlvmBackend_Setup.cpp), bringing strong to 0 but not freeing the
// block while `w`'s weak reference is outstanding; `_ := w;` reaches THIS
// fix's branch and (pre-fix) wild-jumped identically to `sh` above. Looped
// 200x for the same reason: a solo run's leak was invisible to one-shot LSan
// (measured while preparing this test — see the file header).
// per §17.13, §28.9, §28.13 (`Weak<T>`, `Downgrade`) — issue #4224
BEZEL_TEST(using_var_discard_vtableless_builtin_4224,
           weak_using_var_discard_looped_leaks_not_crashes) {
    const std::string prog =
        "module M; "
        "class Widget end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..200 do "
        "      using var sh := new Shared<Widget>(new Widget()); "
        "      using var w := sh.Downgrade(); "
        "      _ := w; "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runValueOnly(prog), 200);
    expectLeakNotCrash(prog);
}

// ── `JsonSerializer` (stdlib §2.15, `System.Serialization`): a second,
// unrelated vtable-less-builtin family in the gap (its runtime shape,
// `bz_jser_t`, is allocated by `bz_jser_alloc` — a different runtime file
// from `Shared`/`Weak`) — pins the fix is general on
// `common::isVtableLessBuiltinClassName`, not `Shared`/`Weak`-specific.
// per §17.13, §28.9, stdlib §2.15 — issue #4224
BEZEL_TEST(using_var_discard_vtableless_builtin_4224,
           jsonserializer_using_var_discard_looped_leaks_not_crashes) {
    const std::string prog =
        "module M; "
        "import System.Serialization; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..200 do "
        "      using var s := new JsonSerializer(); "
        "      _ := s; "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runValueOnly(prog), 200);
    expectLeakNotCrash(prog);
}

// ── `SharedEventSource<T>` (stdlib §6.5): a third, unrelated vtable-less-
// builtin family — its runtime allocation (`bz_shared_event_source_new`,
// `src/runtime/bezel_runtime_events.cpp`) is unrelated to both families
// above, and it is a generic builtin (unlike the other two rows' `<int>`/
// `<Widget>` arguments being incidental, this one's payload type genuinely
// participates in the class name resolution `builtinTypeName` performs).
// per §17.13, §28.9, stdlib §6.5 — issue #4224
BEZEL_TEST(using_var_discard_vtableless_builtin_4224,
           sharedeventsource_using_var_discard_looped_leaks_not_crashes) {
    const std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..200 do "
        "      using var s := new SharedEventSource<int>(); "
        "      _ := s; "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runValueOnly(prog), 200);
    expectLeakNotCrash(prog);
}

// ── REGRESSION GUARDS — the COVERED set (StringBuilder, Mutex) must still
// get a COMPLETE release through `using var` discard: this fix's three-way
// split leaves the `isVtablelessBuiltin && emitVtablelessBuiltinRelease(...)`
// success arm's CONDITION unchanged (only the failure-side `else` was split),
// so these must be bit-for-bit unaffected — clean under ASan/LSan, no leak,
// looped 200x. per §17.13, §28.9, stdlib §3.1 / §6.2 — issue #4224 self-pass
BEZEL_TEST(using_var_discard_vtableless_builtin_4224,
           covered_stringbuilder_using_var_discard_still_releases_looped) {
    const std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..200 do "
        "      using var sb := new StringBuilder(\"x\"); "
        "      _ := sb; "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;";
    expectCleanRelease(prog, 200);
}

BEZEL_TEST(using_var_discard_vtableless_builtin_4224,
           covered_mutex_using_var_discard_still_releases_looped) {
    const std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var i in 1..200 do "
        "      using var m := new Mutex(); "
        "      _ := m; "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Main; "
        "end C;";
    expectCleanRelease(prog, 200);
}

// ── VALUE-ONLY SANITY (no ASan): the primary row's shape must also compile
// and run correctly, once, on the plain build — a leak fix must not change
// the observable VALUE the program computes.
// per §17.13 — issue #4224 self-pass
BEZEL_TEST(using_var_discard_vtableless_builtin_4224,
           shared_using_var_discard_value_only) {
    const std::string prog =
        "module M; "
        "class Widget end Widget; "
        "class C "
        "  public static method Main(): int "
        "    using var sh := new Shared<Widget>(new Widget()); "
        "    _ := sh; "
        "    return 9; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runValueOnly(prog), 9);
}
