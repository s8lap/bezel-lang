// Codegen / leak — reassigning a `string` LOCAL (`a := <string>` where `a`
// already holds a string) leaked the value the local ended up holding. Issue
// #1997.
//
// Spec: §4.6 (`string` is runtime-managed — its memory is reclaimed
// automatically), §28.9 (runtime-managed types; the string refcount discipline),
// §28.3 (local-variable ownership and borrows). Spec revision v0.197.
//
// THE BUG. Every `var` string local owns an independent +1 for its current
// value (a producer initializer TRANSFERS its single reference; a borrow
// initializer is RETAINED). A plain `:=` reassignment (lowerStringLocalReassign)
// releases the displaced OLD value and transfers/retains the NEW one, so the slot
// always owns exactly one reference to whatever it currently holds. But the
// scope-end release is gated on the string-escape walk, which treated ANY
// assignment whose target is the local as an ESCAPE (StringEscapeWalker, the
// deliberately-deferred plain-`:=` arm — the sibling `+=` accumulator arm was
// already relaxed by #1486). A reassigned local was therefore excluded from the
// scope-end release, so the value it held after its LAST reassignment was never
// dropped: it leaked (32-byte header + data buffer), once per reassigned local —
// the "safe direction" leak the old code documented, now closed.
//
// Note the issue title says "leaks the prior value"; the observable is actually
// the FINAL value (the OLD values ARE released, once each, by
// lowerStringLocalReassign — case (B) pins that: a triple reassignment leaks
// exactly ONE string, not two). Either way the fix is the same: make a
// reassigned-but-otherwise-non-escaping string local eligible for the scope-end
// release. The slot's exactly-+1 invariant (maintained across every `:=` by
// lowerStringLocalReassign, retain-before-release so a self-assign never dips rc
// to 0) is the soundness argument the deferred arm was waiting on: the single
// reference is dropped once at scope end — no leak, and no double-free, because
// no other alias exists once the rest of the escape walk is clean.
//
// THE FIX (LlvmBackend_Setup.cpp, StringEscapeWalker::stmt): a plain `:=`
// reassignment of the tracked local is NOT an escape for the string-release
// analysis; the RHS is walked as a safe-read root so a self-assign / self-concat
// / method-alias stays eligible while an occurrence of the target in an escaping
// RHS sub-position (a call arg, a `new`, a lambda capture) still taints it.
// Mirrors the `+=` arm; NOT applied in borrowingArrayMode.
//
// OBSERVABILITY. runAotChecked links + runs the generated code under ASan/LSan;
// a leaked string OR a double-free/UAF (over-releasing a reassigned local that
// still aliases its value, or one that escapes) flips r.ran to false. The exit
// code pins the final string value via `.Length`, so a premature free would
// corrupt it as well as flip r.ran.
//
// Three kinds of case below:
//   * LEAK REPROS (A,B,C,D,G) — LSan-leak on base 741b013, LSan-clean with the
//     string fix (fail-without / pass-with).
//   * DOUBLE-FREE / UAF GUARDS (E,F,H) — LSan-clean BOTH before and after the
//     fix: a self-assign / method-alias reassign, and reassigned locals that
//     ALSO escape via `return` (which must still be excluded from the scope-end
//     release, or the returned +1 would be double-freed). These prove the string
//     fix introduces no double-free.
//   * #1997 BLOCKER regressions (I,J,K) — the string fix relaxes the SHARED
//     StringEscapeWalker; the first attempt (5283b0e) guarded that relaxation with
//     `!borrowingArrayMode`, which leaked it into the dict-materializer (#1028) and
//     grouping (#1930) walks (they run WITHOUT borrowingArrayMode) → a REACHABLE
//     DOUBLE-FREE (ASan SEGV in bz_dict_free / bz_grouping_free). These pin that
//     the relaxation is now OPT-IN (stringReleaseMode), set only at the two string
//     call sites: each program double-frees under the blocker guard and is
//     double-free-clean with the opt-in narrowing. Oracle: ASan with LSan OFF (the
//     reassigned materializer local's ORIGINAL value legitimately leaks — the
//     #1028/#1930 escape-⇒-leak safe direction, present on base too).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::codegen::CodegenOptions;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 11 absorbing rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics and returned from the row on it —
// the definition scripts/check_toolchain_skip_surface.py uses for an
// ABSORBING site. It was also a silent vacuous pass: on a box whose clang
// cannot link the ASan binary, every one of those rows returned WITHOUT
// asserting, and the suite reported a clean PASS having compiled and run
// nothing.
//
// Each guard becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position:
// the skip is still recognised as an environment condition rather than a
// product failure, but it now MARKS THE ROW FAILED instead of hiding it.
// Every substantive assertion in every row — `r.ran` and the exitCode pins —
// stays exactly where it was, and the local predicate is deleted so no
// absorbing shape survives. The guard stays a SEPARATE statement from the
// predicate on purpose: inlining it into an `if` recreates the shape the
// predicate exists to end.

// #1997 BLOCKER oracle — AddressSanitizer with LeakSanitizer OFF.
//
// The dict-materializer (#1028) / grouping (#1930) double-free guards below alias
// a reassigned collection-materializer local onto a value another owner also
// frees; the narrowing this commit adds keeps that reassignment a conservative
// ESCAPE, so the local is excluded from its scope-end free and the value is freed
// exactly once. But a dict/grouping-materializer local has NO release-old-on-
// reassign step (unlike the string path), so the local's ORIGINAL value is
// orphaned by the reassignment and legitimately LEAKS — the #1028/#1930 "reassign
// ⇒ escape ⇒ leak" safe direction, present on base 09ccb4e too. The strict LSan
// oracle (runAotChecked) would therefore flip on that benign residual leak.
//
// This oracle keeps ASan's use-after-free / double-free detection (which is what
// the BLOCKER is — an ASan SEGV in bz_dict_free / bz_grouping_free) while turning
// LSan OFF (detect_leaks=0), so a program that only leaks passes and only a real
// heap error fails. It is exactly runAot's forced-sanitize run configuration
// (opts.sanitizeAddress=true → ASAN_OPTIONS=detect_leaks=0:abort_on_error=0),
// requested per-test so the check is self-contained (not dependent on the gate's
// suite-wide BEZEL_AOT_FORCE_SANITIZE sweep). Delegates to runAotChecked's SKIP
// contract when the clang/ASan toolchain is unavailable.
JitResult runAotAsanNoLeak(std::string_view src) {
    static const bool toolchainOk =
        std::system("clang --version > /dev/null 2>&1") == 0;
    if (!toolchainOk) return runAotChecked(src);  // yields the SKIP diagnostic
    CodegenOptions opts;
    opts.sanitizeAddress = true;  // ASan on; runAot runs it with LSan off
    return runAot(src, opts);
}

// (A) The #1997 repro: a numeric-ToString producer reassigned over another. The
// final "2" leaked its 32-byte header + 2-byte buffer on base.
const char* kReassignNumericToString =
    "module M; class C "
    "  public static method Main(): int "
    "    var a := 1.ToString(); "
    "    a := 2.ToString(); "          // reassign — old "1" released, "2" must not leak
    "    return a.Length; "            // "2".Length = 1
    "  end Main; "
    "end C;";

// (B) Triple reassignment. Pins the OLD-value discipline: the two intermediates
// ("1","22") are each released exactly once by lowerStringLocalReassign, and only
// the FINAL "333" was leaking pre-fix — so this leaks exactly ONE string on base
// (36 bytes), not three, and is clean with the fix.
const char* kTripleReassign =
    "module M; class C "
    "  public static method Main(): int "
    "    var a := 1.ToString(); "
    "    a := 22.ToString(); "
    "    a := 333.ToString(); "
    "    return a.Length; "            // "333".Length = 3
    "  end Main; "
    "end C;";

// (C) A NON-constant concat reassignment (`p + "WW"`, p a runtime local). Proves
// the leak is real for a genuinely heap-allocated producer, not an artifact of
// literal `"Z" + "W"` constant-folding (which mints an immortal string LSan
// ignores). Final "QQWW" (37 bytes) leaked on base.
const char* kNonConstConcatReassign =
    "module M; class C "
    "  public static method Main(): int "
    "    var p := \"QQ\"; "
    "    var a := \"AAAA\" + \"BBBB\"; "
    "    a := p + \"WW\"; "
    "    return a.Length; "            // "QQWW".Length = 4
    "  end Main; "
    "end C;";

// (D) SELF-ASSIGN `a := a` in a non-escaping local. Leaks the held value on base;
// clean with the fix. Doubles as a double-free guard: lowerStringLocalReassign
// retains-before-releases (rc dips +1 then back, never transiently 0), so the
// scope-end release the fix now emits drops the single reference exactly once —
// no UAF, no double-free.
const char* kSelfAssign =
    "module M; class C "
    "  public static method Main(): int "
    "    var a := 42.ToString(); "
    "    a := a; "
    "    return a.Length; "            // "42".Length = 2
    "  end Main; "
    "end C;";

// (E) METHOD-ALIAS reassign `a := a.ToUpper()` — the RHS borrow-reads `a` (the
// receiver is a safe read) and mints a fresh value stored back into `a`. Clean
// BOTH before and after the fix; the guard is that the fix's new scope-end
// release does not double-free the aliased value. Value "HELLO!" (6).
const char* kMethodAliasReassign =
    "module M; class C "
    "  public static method Main(): int "
    "    var a := \"hello\" + \"!\"; "
    "    a := a.ToUpper(); "
    "    return a.Length; "            // "HELLO!".Length = 6
    "  end Main; "
    "end C;";

// (F) A reassigned local that ALSO ESCAPES via `return`. The return transfers the
// local's +1 to the caller, so the local must STAY excluded from the scope-end
// release even though it is reassigned — otherwise the returned reference would be
// double-freed. Clean both before and after the fix (the RHS/return escape walk
// still taints `a`). Main consumes the returned "6".
const char* kEscapingReassignReturn =
    "module M; class C "
    "  public static method Make(n: int): string "
    "    var a := n.ToString(); "
    "    a := (n + 1).ToString(); "    // reassign, then escape via return
    "    return a; "
    "  end Make; "
    "  public static method Main(): int "
    "    return C.Make(5).Length; "    // "6".Length = 1
    "  end Main; "
    "end C;";

// (G) A BORROW-initialized local (`var a := p`, p a parameter) reassigned to a
// producer. Exercises the borrow-candidate path of the escape walk: on base the
// final "5" leaked; with the fix it is released once. Value "5" (1).
const char* kBorrowInitThenReassign =
    "module M; class C "
    "  public static method Run(p: string): int "
    "    var a := p; "
    "    a := 5.ToString(); "
    "    return a.Length; "            // "5".Length = 1
    "  end Run; "
    "  public static method Main(): int "
    "    return C.Run(\"borrowed\"); "
    "  end Main; "
    "end C;";

// (H) SELF-ASSIGN of a local that then ESCAPES via `return`. The strongest
// double-free guard: `a := a` maintains the slot's single +1, and the `return`
// transfers it — so a scope-end release would be a double-free. Clean both before
// and after the fix. Main consumes the returned "7".
const char* kSelfAssignEscapingReturn =
    "module M; class C "
    "  public static method SelfThenReturn(n: int): string "
    "    var a := n.ToString(); "
    "    a := a; "
    "    return a; "
    "  end SelfThenReturn; "
    "  public static method Main(): int "
    "    return C.SelfThenReturn(7).Length; "  // "7".Length = 1
    "  end Main; "
    "end C;";

// =====================================================================
// #1997 BLOCKER cases (#1028 dict / #1930 grouping). The plain-`:=` relaxation
// is SHARED-walker code (StringEscapeWalker), so the first attempt (5283b0e)
// guarded it with `!borrowingArrayMode` — which leaked the relaxation into the
// dict-materializer (#1028) and grouping (#1930) walks that run WITHOUT
// borrowingArrayMode. Those walks have NO release-old-on-reassign step, so a
// reassignment that the relaxation stops treating as an escape leaves the local
// scope-end-FREE-eligible while it aliases a value another owner also frees → a
// REACHABLE DOUBLE-FREE (ASan SEGV in bz_dict_free / bz_grouping_free). This
// commit makes the relaxation OPT-IN (stringReleaseMode), set only at the two
// string call sites, restoring the conservative "reassignment = escape" for the
// dict / grouping / borrowing-array walks. Spec §28.3 (local ownership) / §28.9
// (runtime-managed disposal), v0.197.
//
// Oracle: runAotAsanNoLeak (ASan, LSan off) — each program legitimately leaks the
// reassigned local's ORIGINAL value (the #1028/#1930 escape-⇒-leak safe direction,
// present on base too), so LSan cannot be used; ASan still traps the double-free.
// fail-without (the 5283b0e `!borrowingArrayMode` guard): ASan SEGV, r.ran=false.
// pass-with (this commit's stringReleaseMode opt-in): no double-free, r.ran=true.

// (I) #1028 dict-materializer double-free. `d` is a fresh ToDictionary materializer
// local (dictMaterializerFreeEligible_); `d := u` aliases it onto `u`'s dict, which
// the `using var u` disposes independently. With the reassignment treated as NOT an
// escape (the blocker), `d` stays scope-end-free-eligible → bz_dict_free(d) frees
// u's dict, then u's using-disposal frees it again → SEGV in bz_dict_free. With the
// opt-in narrowing, `d := u` is an escape → d excluded → u's dict freed once. n=3.
const char* kDictAliasReassignDoubleFree =
    "module M; class C "
    "  public static method Main(): int "
    "    var n: int := 0; "
    "    do "
    "      using var src := new Array<int>(); "
    "      src.Add(1); src.Add(2); src.Add(3); "
    "      using var u := src.ToDictionary(lambda(x: int) => x); "
    "      var d := src.ToDictionary(lambda(x: int) => x); "
    "      d := u; "                     // alias onto the using-owned dict
    "      n := d.Count; "               // 3
    "    end; "
    "    return n; "
    "  end Main; "
    "end C;";

// (J) #1028 dict — TWO materializer locals aliased onto one using-owned dict. Both
// `d` and `e` reassigned to `keep`; with the blocker guard each stays eligible →
// bz_dict_free(d) + bz_dict_free(e) + keep's using-disposal all free the same dict
// → double/triple-free. With the narrowing both are escapes → keep freed once.
// n = d.Count + e.Count = 2 + 2 = 4.
const char* kDictTwoAliasReassignDoubleFree =
    "module M; class C "
    "  public static method Main(): int "
    "    var n: int := 0; "
    "    do "
    "      using var src := new Array<int>(); "
    "      src.Add(1); src.Add(2); "
    "      using var keep := src.ToDictionary(lambda(x: int) => x); "
    "      var d := src.ToDictionary(lambda(x: int) => x); "
    "      var e := src.ToDictionary(lambda(x: int) => x); "
    "      d := keep; "
    "      e := keep; "
    "      n := d.Count + e.Count; "     // 2 + 2
    "    end; "
    "    return n; "
    "  end Main; "
    "end C;";

// (K) #1930 grouping double-free. `g` is a bare-GroupBy element-terminal local
// (groupingFreeEligible_); `g := arr[0]` aliases it onto a grouping the owning
// `Array<IGrouping>` `arr` (a `using var`) owns and disposes. With the blocker
// guard `g` stays eligible → bz_grouping_free(g) frees arr[0]'s grouping, then arr's
// disposal frees it again → SEGV in bz_grouping_free / bz_array_free. With the
// narrowing `g := arr[0]` is an escape → g excluded → arr frees its groupings once.
// xs = [1,3,2] → GroupBy(x mod 2): key 1 {1,3}, key 0 {2}; arr[0] = key-1 group,
// Count() = 2.
const char* kGroupingAliasReassignDoubleFree =
    "module M; "
    "import System.Collections.Linq; "
    "class C "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    do "
    "      using var xs := new Array<int>(); "
    "      xs.Add(1); xs.Add(3); xs.Add(2); "
    "      using var arr := xs.GroupBy(lambda(x: int) => x mod 2).ToArray(); "
    "      var g := xs.GroupBy(lambda(x: int) => x mod 2).First(); "
    "      g := arr[0]; "                // alias onto a grouping arr owns
    "      total := g.Count(); "         // key-1 group {1,3} → 2
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// LEAK REPROS — LSan-leak on base 741b013, LSan-clean with the fix.
// §4.6 / §28.9 / §28.3 (v0.197).
// =====================================================================
BEZEL_TEST(string_local_reassign_leak_1997,
           reassign_numeric_tostring_lsan_clean_spec_28_9) {
    JitResult r = runAotChecked(kReassignNumericToString);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // final "2" freed once (was leaked pre-fix)
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // "2".Length
}

BEZEL_TEST(string_local_reassign_leak_1997,
           triple_reassign_intermediates_released_once_spec_28_9) {
    JitResult r = runAotChecked(kTripleReassign);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // "1","22" released once each; final "333" freed
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // "333".Length
}

BEZEL_TEST(string_local_reassign_leak_1997,
           nonconst_concat_reassign_lsan_clean_spec_4_6) {
    JitResult r = runAotChecked(kNonConstConcatReassign);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // final heap "QQWW" freed once (not literal-folded)
    BEZEL_EXPECT_EQ(r.exitCode, 4);  // "QQWW".Length
}

BEZEL_TEST(string_local_reassign_leak_1997,
           self_assign_lsan_clean_no_double_free_spec_28_3) {
    JitResult r = runAotChecked(kSelfAssign);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // clean: no leak, no double-free (retain-before-release)
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // "42".Length
}

BEZEL_TEST(string_local_reassign_leak_1997,
           borrow_init_then_reassign_lsan_clean_spec_28_9) {
    JitResult r = runAotChecked(kBorrowInitThenReassign);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // borrow-candidate path: final "5" freed once
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // "5".Length
}

// =====================================================================
// DOUBLE-FREE / UAF GUARDS — LSan-clean BOTH before and after the fix.
// Prove the fix's new scope-end release introduces no double-free / UAF.
// §28.3 / §28.9 (v0.197).
// =====================================================================
BEZEL_TEST(string_local_reassign_leak_1997,
           method_alias_reassign_no_double_free_spec_28_3) {
    JitResult r = runAotChecked(kMethodAliasReassign);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // `a := a.ToUpper()` — no double-free of the aliased value
    BEZEL_EXPECT_EQ(r.exitCode, 6);  // "HELLO!".Length
}

BEZEL_TEST(string_local_reassign_leak_1997,
           escaping_reassigned_return_no_double_free_spec_28_3) {
    JitResult r = runAotChecked(kEscapingReassignReturn);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // reassigned + returned: transferred +1 not double-freed
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // "6".Length
}

BEZEL_TEST(string_local_reassign_leak_1997,
           self_assign_escaping_return_no_double_free_spec_28_3) {
    JitResult r = runAotChecked(kSelfAssignEscapingReturn);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // self-assign + returned: single +1 transferred, not double-freed
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // "7".Length
}

// =====================================================================
// #1997 BLOCKER regressions — the shared StringEscapeWalker relaxation must NOT
// leak into the dict-materializer (#1028) / grouping (#1930) walks. Each program
// double-frees (ASan SEGV in bz_dict_free / bz_grouping_free) under the first
// attempt's `!borrowingArrayMode` guard, and is double-free-CLEAN under this
// commit's stringReleaseMode opt-in. Oracle: ASan with LSan OFF (the reassigned
// materializer local's original value legitimately leaks — the escape-⇒-leak safe
// direction). §28.3 / §28.9 (v0.197).
// =====================================================================
BEZEL_TEST(string_local_reassign_leak_1997,
           dict_materializer_alias_reassign_no_double_free_spec_28_9) {
    JitResult r = runAotAsanNoLeak(kDictAliasReassignDoubleFree);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // #1028: no bz_dict_free double-free (blocker guard SEGVs here)
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // d.Count
}

BEZEL_TEST(string_local_reassign_leak_1997,
           dict_materializer_two_alias_reassign_no_double_free_spec_28_9) {
    JitResult r = runAotAsanNoLeak(kDictTwoAliasReassignDoubleFree);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // #1028: two aliases onto one using-owned dict, freed once
    BEZEL_EXPECT_EQ(r.exitCode, 4);  // d.Count + e.Count
}

BEZEL_TEST(string_local_reassign_leak_1997,
           grouping_alias_reassign_no_double_free_spec_28_9) {
    JitResult r = runAotAsanNoLeak(kGroupingAliasReassignDoubleFree);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);        // #1930: no bz_grouping_free double-free (blocker guard SEGVs here)
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // key-1 group {1,3}, Count()
}
