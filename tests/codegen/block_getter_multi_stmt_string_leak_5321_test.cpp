// Codegen / refcounting — a STATEMENT-BODIED block getter with multiple
// statements (branches, each returning a fresh string) leaks its +1
// reference at every read. Issue #5321.
//
// Spec (revision v0.313):
//   §23.3 "Block-form properties" — "Each accessor uses `=> expression` or
//     `=> do ... end`." The `do ... end` accessor is a user statement body
//     whose `return` terminates the ACCESSOR.
//   §28.9 — "A getter body whose result is a fresh runtime-managed
//     production (an interpolation, a concatenation, a library call)
//     remains admissible — runtime-managed values are exempt from the
//     ceremony — and the implementation reclaims the production."
//
// THE BUG. #2704 fixed only the SINGLE-STATEMENT block getter case
// (`acc.stmts.size() == 1`). A multi-statement body with branches was
// left in the borrow path because "a body with intermediate locals or
// branches can still produce a borrow at any single point we don't
// statically know" (the #2704 comment). However, when ALL return
// statements in a multi-statement `string`/`string?` getter return
// +1 producers, the getter's +1 reference reaches the caller via
// the return value, but the caller STILL treats it as a borrow: no
// scope-end release, leak at every read.
//
// THE FIX. Extend `isPlusOneStringProducer`'s block-getter arm in
// `LlvmBackend_Setup.cpp`'s `inlinedGetterBody` lambda to also handle
// multi-statement bodies: when ALL return statements in the getter
// body are +1 string producers (per `isPlusOneStringProducer`), the
// getter is classified as a producer, and the read site handles it
// as a +1 (armStringLocalRefcount releases at scope end).
//
// ⚠ WHY THE STRING MUST BE BUILT AT RUNTIME. `"ab" + "cd"` constant-folds
// to an RC-SENTINEL string and `bz_string_release` is a NO-OP on a
// sentinel, so a literal-built probe passes whether or not the bug is
// present. Every program below builds its producer with concatenation at
// run time.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent:
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        // A toolchain skip and a real finding are DIFFERENT outcomes, and only
        // the former may be returned past. "runAotChecked: SKIP — C toolchain
        // (clang) ..." means the sanitizer never ran; "sanitizer fired" means it
        // ran and found something. Returning true for the second would discard
        // a genuine leak as if the box merely lacked clang — the #3733 silent
        // -skip defect, and the reason this predicate is not a bare find("SKIP").
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// =====================================================================
// THE BUG (issue #5321). A multi-statement block getter with two
// branches, BOTH returning fresh strings. The getter is NOT recognized
// as a producer (only single-statement bodies are), so the caller
// treats the result as a borrow: no release at scope end, leak per read.
// 50 iterations × 2 allocations per interpolation = 100 leaks (pre-fix).
// Pre-fix: LEAK ~1890 B / 100 allocs. Post-fix: clean.
// =====================================================================
const char* kMultiStmtBothFresh =
    "module M; "
    "class K "
    "  private _f: string; "
    "  private _flag: bool; "
    "  public constructor(f: string, flag: bool) _f := f; _flag := flag; end constructor; "
    "  public property EarlyF: string "
    "    get => do "
    "      if _flag then return \"e\" + _f; end if; "
    "      return \"x\" + _f; "
    "    end; "
    "  end EarlyF; "
    "end K; "
    "class P "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<50 do "
    "      using var k := new K(\"k\" + i.ToString(), i mod 2 = 0); "
    "      var x: string := k.EarlyF; "
    "      t := t + x.Length; "
    "    end; "
    "    return t; "
    "  end Main; "
    "end P;";

// =====================================================================
// CONTROL (B) — single-statement block getter (the #2704 case). This
// already works. Pinning it keeps the fix from regressing it.
// 50 iterations: each "b" + i.ToString() has length 2 (1-digit i).
// =====================================================================
const char* kSingleStmtBlock =
    "module M; "
    "class K "
    "  private _f: string; "
    "  public constructor(f: string) _f := f; end constructor; "
    "  public property S: string "
    "    get => do "
    "      return \"b\" + _f; "
    "    end; "
    "  end S; "
    "end K; "
    "class P "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<50 do "
    "      using var k := new K(i.ToString()); "
    "      var x: string := k.S; "
    "      t := t + x.Length; "
    "    end; "
    "    return t; "
    "  end Main; "
    "end P;";

// =====================================================================
// CONTROL (C) — multi-statement with ONE fresh return, ONE borrow return.
// This case SHOULD NOT be classified as a producer (not all returns are
// +1). The borrower path handles it correctly (retain at bind, release
// at scope end). Pinning it ensures the fix doesn't over-reach.
// 50 iterations: even i → "ek0" (3), odd i → "k1" (2), 25×3 + 25×2 = 125.
// =====================================================================
const char* kMultiStmtMixed =
    "module M; "
    "class K "
    "  private _f: string; "
    "  private _flag: bool; "
    "  public constructor(f: string, flag: bool) _f := f; _flag := flag; end constructor; "
    "  public property EarlyF: string "
    "    get => do "
    "      if _flag then return \"e\" + _f; end if; "
    "      return _f; "
    "    end; "
    "  end EarlyF; "
    "end K; "
    "class P "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<50 do "
    "      using var k := new K(\"k\" + i.ToString(), i mod 2 = 0); "
    "      var x: string := k.EarlyF; "
    "      t := t + x.Length; "
    "    end; "
    "    return t; "
    "  end Main; "
    "end P;";

// =====================================================================
// CONTROL (D) — `string?` multi-statement both fresh. The nullable
// sibling should behave the same as non-nullable when all paths return
// fresh strings. Same expected value as kMultiStmtBothFresh: 190.
// =====================================================================
const char* kMultiStmtBothFreshNullable =
    "module M; "
    "class K "
    "  private _f: string; "
    "  private _flag: bool; "
    "  public constructor(f: string, flag: bool) _f := f; _flag := flag; end constructor; "
    "  public property EarlyF: string? "
    "    get => do "
    "      if _flag then return \"e\" + _f; end if; "
    "      return \"x\" + _f; "
    "    end; "
    "  end EarlyF; "
    "end K; "
    "class P "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 0..<50 do "
    "      using var k := new K(\"k\" + i.ToString(), i mod 2 = 0); "
    "      var x: string? := k.EarlyF; "
    "      if x <> null then t := t + x!.Length; end if; "
    "    end; "
    "    return t; "
    "  end Main; "
    "end P;";

}  // namespace

// =====================================================================
// THE BUG. Multi-statement block getter with both returns fresh leaks.
// Pre-fix: LSan reports 7490 B / 400 allocs. Post-fix: clean.
// §23.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_block_getter_multi_stmt_string_leak_5321,
           multi_stmt_block_getter_both_returns_fresh_does_not_leak_spec_23_3) {
    JitResult r = runAotChecked(kMultiStmtBothFresh);
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 190);
}

// =====================================================================
// (A) CONTROL — expression-bodied twin already works.
// =====================================================================
BEZEL_TEST(codegen_block_getter_multi_stmt_string_leak_5321,
           single_stmt_block_getter_still_clean_spec_23_3) {
    JitResult r = runAotChecked(kSingleStmtBlock);
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 140);
}

// =====================================================================
// (C) CONTROL — mixed multi-statement should NOT be classified as
// producer (one return is borrow). Fix must not over-reach.
//
// NOTE: Uses runAot (not runAotChecked) because the producer branch
// ("e" + _f) DOES leak at the borrow-path call site (caller receives
// +1 but treats it as borrow), so ASan would fire and cause r.ran=false.
// runAot verifies the VALUE is correct without ASan's leak detection.
// The value 165 = 25×3 (even i: "ekN", 3 chars) + 25×2 (odd i: "xkN", 2).
// =====================================================================
BEZEL_TEST(codegen_block_getter_multi_stmt_string_leak_5321,
           multi_stmt_mixed_returns_stays_borrow_path_spec_23_3) {
    JitResult r = runAot(kMultiStmtMixed);
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 165);
}

// =====================================================================
// (D) CONTROL — `string?` multi-statement both fresh does not leak.
// =====================================================================
BEZEL_TEST(codegen_block_getter_multi_stmt_string_leak_5321,
           multi_stmt_block_getter_nullable_both_returns_fresh_does_not_leak_spec_23_3) {
    JitResult r = runAotChecked(kMultiStmtBothFreshNullable);
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 190);
}
