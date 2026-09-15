// Codegen — issue #4234: a compile-time denial-of-service on wide/deep
// generic-record "diamonds" — declaring records where each level's fields
// all repeat the SAME nested record type (`Ri<T>` with `branch` fields all
// of type `R(i-1)<T>`), branch=3/depth=6, took 75.1s wall / 73.2s user CPU /
// 1.5GB RSS on a program that only DECLARES the types (zero uses, no
// `where T: new()`, no `default(T)`).
//
// MEASURED ROOT CAUSE (this run's own instrumentation, NOT the issue's own
// guess): the issue suspected `Resolver::recordValueContainsSelf` or
// `common::isBorrowBearingRecordCommon` (both declaration-time sema
// checks). Phase-by-phase timers proved `Resolver::run()`'s ENTIRE set of
// declaration-time checks — including both suspects — completes in under
// 1ms; `sema::resolveTypes`/`typecheckExpressions`/`checkOwnership` etc.
// were likewise instant. The cost was 100% inside `codegen::emitObjectFile`
// (specifically LLVM's own `pm.run(module)` codegen pass), because the
// emitted IR itself was exponentially large: one `bz$boxtostr$R4` function
// alone was 122,545 LLVM IR lines at branch=3/depth=4 (measured via
// BEZEL_DUMP_IR), growing ~3x per added level.
//
// The actual bug: `Emitter::emitValueToString`'s Record-kind arm
// (LlvmBackend_String.cpp) called `emitRecordToString(val, ts)` directly —
// plain C++ recursion with NO memoization — for every record-typed FIELD.
// A record type reached via `branch` sibling field positions therefore
// re-expanded its ENTIRE recursive field-listing body at every occurrence:
// O(branch^depth) LLVM instructions from a program that only declares the
// types. `Emitter::emitValueEquality` (LlvmBackend_Layout.cpp, calling
// `emitStructEquality`) and `Emitter::emitValueHashCode`
// (LlvmBackend_String.cpp, calling `emitRecordHashCode`) shared the
// IDENTICAL unmemoized-recursion shape — smaller per-node cost (an AND-
// chain / hash-fold vs a string-concat chain) but the same asymptotic
// growth: with ONLY the ToString arm fixed, an unfixed depth=8/branch=3
// diamond alone still cost 167s (measured), confirming the defect class
// reproduces one level of nesting later through the sibling arms.
//
// THE FIX (§9.1, v0.298): each of the three arms now routes its NESTED
// (field-level) recursion through a per-TypeSymbol MEMOIZED helper function
// (`recordToStringHelperFn` / `recordEqualityHelperFn` /
// `recordHashCodeHelperFn`, LlvmBackend_Setup.cpp) — a real LLVM `call`
// instead of re-inlining the recursive body. A record type is now built
// ONCE regardless of how many sibling positions reach it, collapsing the
// growth from O(branch^depth) to O(number of distinct record types
// declared). Measured post-fix: branch=3/depth=6 compiles in ~3s (down
// from 75-87s), and branch=3/depth=8 (which reproduced 167s with only the
// ToString arm fixed) also stays in the low single digits of seconds.
//
// §9.1: "ToString formatted as `"TypeName(Field1: value1, Field2: value2)"`."
// §8.5 "ToString": "Each component renders as its own `ToString` renders
// it (virtual dispatch …)" — the recursive clause this fix's memoization
// must preserve exactly, not just avoid re-expanding.
// §8.3 "Equals and GetHashCode": "Positional record | Field-by-field value
// equality … | Hash of all positional fields" and "`GetHashCode` must
// satisfy: if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`."
//
// REVIEW SEND-BACK (rev/4234 -> fixback/4234, review of 383cadb): row (D)
// below originally asserted a 20s WALL-CLOCK bound around `runAot`. The
// reviewer measured 373.43s (18.7x over) for this exact shape under organic
// multi-lane box contention — 2 of 17 samples over the bound, box state
// `vmstat b=163-166 wa=93-99% free=315-430Mi` (CLAUDE.md's gate-sharding /
// AOT-process-isolation contention scar, the #3010 class: the sharded
// runner can over-report / run arbitrarily slower under fork+link
// contention — a real box state, not a harness bug). Row (D) now asserts a
// STRUCTURAL, contention-independent bound instead: total LLVM IR TEXT SIZE
// of the compiled module (via `BEZEL_DUMP_IR`) — the exact signal this
// fix's own root-cause analysis used to find the bug in the first place
// (see "MEASURED ROOT CAUSE" above). Wall-clock is kept as a PRINT-ONLY
// informational line (never asserted) so a real perf cliff still shows up
// in test output without being able to flake the gate.
//
// Also added per review: controls (E)/(F) below, covering the
// cross-instantiation memoization-key axis (the review-caught defect in
// this commit's own history, previously pinned only for `Equals` by the
// pre-existing tests/codegen/boxed_generic_record_equals_2176_test.cpp).

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Builds the exact "diamond" shape from the issue's own repro generator
// (gen_diamond_decl_only.py): `R0<T>(V: T)`, then `Ri<T>` with `branch`
// fields all of type `R(i-1)<T>`, for i = 1..depth. Declarations only — no
// instantiation, no `where T: new()`, no `default(T)` — matching the
// issue's finding that the cost is ENTIRELY in declaration processing.
std::string makeDiamondDecl(int branch, int depth, const char* moduleName) {
    std::string src = std::string("module ") + moduleName + "; ";
    src += "public record R0<T>(V: T); ";
    for (int i = 1; i <= depth; ++i) {
        src += "public record R" + std::to_string(i) + "<T>(";
        for (int j = 0; j < branch; ++j) {
            if (j != 0) src += ", ";
            src += "F" + std::to_string(j) + ": R" + std::to_string(i - 1) +
                   "<T>";
        }
        src += "); ";
    }
    src += "public class ProgDPX "
           "public static method Main(): int return 0; end Main; "
           "end ProgDPX;";
    return src;
}

// Resolves the sibling `bezelc` binary next to this test binary, exactly as
// tests/codegen/case_stmt_total_dead_edge_trap_3073_test.cpp's own
// `bezelcPath()` does (duplicated here rather than shared — neither file
// exposes it via a header): `<build>/tests/bezel_tests_codegen` (this
// process) and `<build>/bezelc` are siblings one level up. Resolved via
// /proc/self/exe so this works from any worktree's own build directory.
//
// ⚠ LANE_RULES §23: this shells out to the STANDALONE `build/bezelc`
// executable, not the in-process compiler library the rest of this test
// binary links against. After touching any codegen source, rebuild the
// `bezelc` TARGET explicitly (`cmake --build build --target bezelc`) before
// trusting row (D) below — the test-binary build alone will not refresh it.
std::string bezelcPath() {
    std::array<char, 4096> buf{};
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return "";
    std::filesystem::path exe(
        std::string(buf.data(), static_cast<std::size_t>(n)));
    return (exe.parent_path().parent_path() / "bezelc").string();
}

// Compiles `source` to an object (no link, no Main required) with
// BEZEL_DUMP_IR=1 set, capturing the LLVM module text the compiler prints to
// stderr (LlvmBackend.cpp: `if (... getenv("BEZEL_DUMP_IR")) m->print(errs(),
// ...)`) — the same mechanism case_stmt_total_dead_edge_trap_3073_test.cpp's
// compileAndCaptureIR uses. Runs the real `bezelc` binary as a SUBPROCESS
// (popen) rather than calling Driver in-process: LLVM's `errs()` is a raw
// fd_ostream, and capturing it by redirecting this test process's own fd 2
// around the call is unreliable (buffering/timing, per that file's own
// hand-off note). Returns "" on any failure to locate/run bezelc — the
// caller treats an empty result as a hard failure, never confused with "0
// lines of IR" by the size check below.
std::string compileDiamondAndCaptureIR(const std::string& source) {
    static int counter = 0;
    std::string bezelc = bezelcPath();
    if (bezelc.empty() || !std::filesystem::exists(bezelc)) return "";
    auto dir = std::filesystem::temp_directory_path();
    std::string stem = "bezel_4234_ir_" + std::to_string(counter++);
    std::string srcPath = (dir / (stem + ".bzl")).string();
    std::string objPath = (dir / (stem + ".o")).string();
    {
        std::ofstream out(srcPath);
        out << source;
    }
    std::string cmd = "BEZEL_DUMP_IR=1 '" + bezelc + "' -c -o '" + objPath +
                      "' '" + srcPath + "' 2>&1";
    std::string text;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        std::array<char, 65536> chunk{};
        std::size_t got;
        while ((got = fread(chunk.data(), 1, chunk.size(), p)) > 0) {
            text.append(chunk.data(), got);
        }
        pclose(p);
    }
    std::remove(srcPath.c_str());
    std::remove(objPath.c_str());
    return text;
}

// (A) CONTROL — §9.1/§8.5 correctness: a small (non-exponential) diamond's
// ToString still recurses into the SAME field type from two sibling
// positions and renders each independently. Must be UNCHANGED by the fix
// (the helper function must produce byte-identical output to the old
// inline recursion).
const char* kNestedToString =
    "module M4234A; "
    "public record Inner(V: int); "
    "public record Outer(A: Inner, B: Inner); "
    "public class C "
    "  public static method Main(): int "
    "    var o := new Outer(new Inner(1), new Inner(2)); "
    "    Console.WriteLine(o.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (B) CONTROL — §8.3/§8.4 correctness: nested-record `=` (routed through
// emitValueEquality -> emitStructEquality, now via recordEqualityHelperFn)
// must still distinguish equal from unequal nested field values.
const char* kNestedEquals =
    "module M4234B; "
    "public record Inner(V: int); "
    "public record Outer(A: Inner, B: Inner); "
    "public class C "
    "  public static method Main(): int "
    "    var o1 := new Outer(new Inner(1), new Inner(2)); "
    "    var o2 := new Outer(new Inner(1), new Inner(2)); "
    "    var o3 := new Outer(new Inner(1), new Inner(3)); "
    "    Console.WriteLine((o1 = o2).ToString()); "
    "    Console.WriteLine((o1 = o3).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (C) CONTROL — §8.3's hash-equality invariant ("if a.Equals(b) then
// a.GetHashCode() = b.GetHashCode()") over a nested record, routed through
// emitValueHashCode -> emitRecordHashCode -> recordHashCodeHelperFn.
const char* kNestedHashConsistency =
    "module M4234C; "
    "public record Inner(V: int); "
    "public record Outer(A: Inner, B: Inner); "
    "public class C "
    "  public static method Main(): int "
    "    var o1 := new Outer(new Inner(1), new Inner(2)); "
    "    var o2 := new Outer(new Inner(1), new Inner(2)); "
    "    Console.WriteLine((o1.GetHashCode() = o2.GetHashCode()).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (E) CONTROL — cross-instantiation layout, the REVIEW-CAUGHT DEFECT this
// commit's own history describes: a nested GENERIC record reached via TWO
// DIFFERENT concrete instantiations of the SAME record symbol (`Box<T>`'s
// OUTER instantiation `Box<Box<int>>` and its own field's INNER
// instantiation `Box<int>`) must render each instantiation through ITS OWN
// correct layout, not a function cached for a DIFFERENT instantiation's
// memory shape. Controls (A)-(C) above are non-generic (`Inner`/`Outer`)
// and cannot exercise this axis at all — only ONE `recTy` is ever reached
// for `Inner`, so a keying bug that conflates two DIFFERENT `recTy`'s of
// the SAME symbol is invisible to them. The only PRE-EXISTING test on this
// axis is tests/codegen/boxed_generic_record_equals_2176_test.cpp's
// nested_generic_record_equal_9_1 — same `Box<Box<int>>` shape, but only
// via `Equals`; this adds the `ToString` sibling with DIRECT (non-boxed)
// dispatch, matching (A)-(C)'s own convention.
//
// EMPIRICALLY VERIFIED to catch the reviewed bug (not merely by analogy):
// reconstructing the commit's own described first draft — keying
// `recordToStringHelperFns_`/`recordEqualityHelperFns_`/
// `recordHashCodeHelperFns_` on a bare `TypeSymbol*` and re-deriving the
// struct type in-function via `recordStruct(rec)` under whatever ambient
// `genericSubst_` happens to be active, instead of trusting the
// caller-supplied concrete `recTy` — and rebuilding `bezelc` from that
// reconstruction makes this EXACT program SEGFAULT (measured on this box:
// exit 139; restored to the real fix and rebuilt afterward, confirmed via
// `nm build/bezelc | grep record.*HelperFn` showing all 3 symbols present
// again). The real fix instead produces exactly
// "Box(value: Box(value: 7))\nBox(value: Box(value: 9))".
//
// NOT affected by the pre-existing, out-of-scope #4251 gap (a DIFFERENT
// bug: a nested generic-record field over a REFERENCE type like `string`
// loses content-equality/hash because `recordMemberTypeResolved` returns
// an unresolved bare `T` with no active `RecordSubstGuard` at the
// nested-recursion call sites). This control uses `int`, a value type
// whose raw bits directly encode the value — #4251's own issue text notes
// this is why that gap does not manifest for non-reference fields, and the
// pre-existing nested_generic_record_equal_9_1 control (also `int`-only)
// already demonstrates this axis is unaffected by #4251.
const char* kNestedGenericCrossInstantiationToString =
    "module M4234E; "
    "public record Box<T>(value: T); "
    "public class C "
    "  public static method Main(): int "
    "    var b1 := new Box<Box<int>>(new Box<int>(7)); "
    "    var b2 := new Box<Box<int>>(new Box<int>(9)); "
    "    Console.WriteLine(b1.ToString()); "
    "    Console.WriteLine(b2.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (F) CONTROL — the GetHashCode sibling of (E). Two same-inner-value nested
// boxes must hash EQUAL (the §8.3 invariant: a.Equals(b) => equal hash);
// two DIFFERENT-inner-value nested boxes are ALSO checked for a DIFFERENT
// hash — not a spec guarantee on its own (§8.3 only requires the converse),
// but included specifically because #4251 documents that a hash function
// degraded to a CONTENT-INDEPENDENT CONSTANT still vacuously satisfies an
// equal-implies-equal-hash-only check (LANE_RULES: "a trap for exactly the
// kind of 'both sides agree' control row" the project warns about). b1 vs
// b3 differ only in the doubly-nested int, so a non-constant hash is
// expected to differ. EMPIRICALLY VERIFIED alongside (E): this exact
// program also SEGFAULTs (exit 139) under the reconstructed review-caught
// bug and produces "True\nFalse" under the real fix — see (E)'s comment for
// the full reconstruction/restore method.
const char* kNestedGenericCrossInstantiationHash =
    "module M4234F; "
    "public record Box<T>(value: T); "
    "public class C "
    "  public static method Main(): int "
    "    var b1 := new Box<Box<int>>(new Box<int>(7)); "
    "    var b2 := new Box<Box<int>>(new Box<int>(7)); "
    "    var b3 := new Box<Box<int>>(new Box<int>(9)); "
    "    Console.WriteLine((b1.GetHashCode() = b2.GetHashCode()).ToString()); "
    "    Console.WriteLine((b1.GetHashCode() = b3.GetHashCode()).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// (A) CONTROL: nested-record ToString field listing is unaffected by the fix.
// ===========================================================================
BEZEL_TEST(codegen_nested_record_diamond_declaration_time_dos_4234,
           nested_record_tostring_field_listing_9_1_8_5) {
    BEZEL_EXPECT_EQ(runAotOut(kNestedToString), "Outer(A: Inner(V: 1), B: Inner(V: 2))");
}

// ===========================================================================
// (B) CONTROL: nested-record `=` still distinguishes equal / unequal values.
// ===========================================================================
BEZEL_TEST(codegen_nested_record_diamond_declaration_time_dos_4234,
           nested_record_equals_field_by_field_8_3) {
    BEZEL_EXPECT_EQ(runAotOut(kNestedEquals), "True\nFalse");
}

// ===========================================================================
// (C) CONTROL: the §8.3 hash-equality invariant holds over a nested record.
// ===========================================================================
BEZEL_TEST(codegen_nested_record_diamond_declaration_time_dos_4234,
           nested_record_hash_equality_invariant_8_3) {
    BEZEL_EXPECT_EQ(runAotOut(kNestedHashConsistency), "True");
}

// ===========================================================================
// (E) CONTROL: cross-instantiation ToString — see comment above kNested-
// GenericCrossInstantiationToString.
// ===========================================================================
BEZEL_TEST(codegen_nested_record_diamond_declaration_time_dos_4234,
           nested_generic_record_cross_instantiation_tostring_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(kNestedGenericCrossInstantiationToString),
                    "Box(value: Box(value: 7))\nBox(value: Box(value: 9))");
}

// ===========================================================================
// (F) CONTROL: cross-instantiation GetHashCode — see comment above kNested-
// GenericCrossInstantiationHash.
// ===========================================================================
BEZEL_TEST(codegen_nested_record_diamond_declaration_time_dos_4234,
           nested_generic_record_cross_instantiation_hash_8_3) {
    BEZEL_EXPECT_EQ(runAotOut(kNestedGenericCrossInstantiationHash),
                    "True\nFalse");
}

// ===========================================================================
// (D) THE PIN: branch=3/depth=6 declaration-only diamond (the issue's own
// measured repro shape) must stay LINEAR, not exponential, in emitted LLVM
// IR — the actual mechanism this fix repairs (see "MEASURED ROOT CAUSE" and
// "THE FIX" at the top of this file).
//
// ORACLE HISTORY: this row originally asserted a 20s WALL-CLOCK bound
// around `runAot`. Review measured 373.43s (18.7x over the bound) for this
// exact shape under organic multi-lane box contention on this project's
// shared box — 2 of 17 samples over threshold, `vmstat b=163-166
// wa=93-99% free=315-430Mi` — while a quiet box gave 1.5-1.9s. A wall-clock
// bound cannot distinguish "the fix regressed" from "the box is thrashing"
// (CLAUDE.md's gate-sharding / #3010-class contention scar: the sharded
// AOT runner can over-report or simply run much slower under fork+link
// contention). This row now asserts the STRUCTURAL signal instead: total
// LLVM IR TEXT SIZE of the compiled module, captured via `BEZEL_DUMP_IR`
// through a real `bezelc` subprocess (`compileDiamondAndCaptureIR` above,
// same technique as case_stmt_total_dead_edge_trap_3073_test.cpp) — the
// exact signal this fix's own root-cause analysis used to find the bug
// (file header: "one bz$boxtostr$R4 function alone was 122,545 LLVM IR
// lines at branch=3/depth=4"). Wall-clock is kept below as a PRINT-ONLY
// informational line — it can never fail this test.
//
// MEASURED (this box, this worktree, via
// `BEZEL_DUMP_IR=1 bezelc -c -o <obj> <src>.bzl 2>&1 | wc -l`, total dumped-
// module line count for THIS generator's output at each depth):
//   branch=3/depth=4: pre-fix   186,278 lines  /  post-fix  16,262 lines
//   branch=3/depth=5: pre-fix   565,324 lines  /  post-fix  20,090 lines
//   branch=3/depth=6: pre-fix 1,704,231 lines  /  post-fix  23,918 lines
// (pre-fix growth ~3.0x per level, matching "growing ~3x per level" above;
// post-fix growth ~1.2x per level — linear in the number of DISTINCT record
// types declared, not exponential in branch^depth). Pre-fix figures were
// measured by reverting LlvmBackend_Layout.cpp/_Setup.cpp/_String.cpp/
// _impl.hpp to this branch's parent (6f46a73) in this SAME worktree,
// rebuilding `bezelc`, measuring, then restoring and rebuilding again
// (LANE_RULES §3 revert-then-rebuild-then-restore; the restored `bezelc`
// was verified via `nm build/bezelc | grep record.*HelperFn` showing all 3
// fix symbols present again before any further measurement, and
// `build/bezelc --version` showed the unchanged `g383cadb` describe string
// throughout — expected per LANE_RULES §19's amendment, since
// BEZEL_GIT_DESCRIBE is baked at CMake CONFIGURE time and this was a
// rebuild, not a reconfigure; content was verified via the symbol check
// instead of the version string).
//
// THRESHOLD 150,000: ~6.3x above the measured post-fix figure at this
// exact shape (23,918) — comfortable headroom for incidental IR-text
// growth (new stdlib boilerplate, an LLVM version bump changing IR-text
// formatting) — and >11x BELOW the measured pre-fix figure at this same
// shape (1,704,231). It is also below the SMALLEST fully-unmemoized figure
// measured at any depth in this table (186,278, at depth=4 — two levels
// shallower than what this row asserts on), so even a regression that only
// re-breaks ONE of the three arms (ToString XOR Equals XOR GetHashCode) at
// depth=6 is expected to clear it by a wide margin: the mechanism is the
// identical exponential re-expansion in whichever arm regresses, and the
// file header's own measurement independently confirms this ("with ONLY
// the ToString arm fixed, an unfixed depth=8/branch=3 diamond alone still
// cost 167s").
BEZEL_TEST(codegen_nested_record_diamond_declaration_time_dos_4234,
           branch3_depth6_diamond_declaration_only_bounded_ir_size_4234) {
    std::string src = makeDiamondDecl(/*branch=*/3, /*depth=*/6, "M4234DiamondPerf");

    // Functional correctness + wall-clock as an INFORMATIONAL PRINT ONLY
    // (see ORACLE HISTORY above) — never asserted, so box contention cannot
    // flake this test.
    auto t0 = std::chrono::steady_clock::now();
    JitResult r = runAot(src);
    auto t1 = std::chrono::steady_clock::now();
    double elapsedS = std::chrono::duration<double>(t1 - t0).count();
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::cout << "    #4234 informational wall-clock: " << elapsedS
              << "s (NOT asserted — see ORACLE HISTORY above; the "
                 "structural IR-size bound below is the real oracle)\n";

    // THE ACTUAL ORACLE: total LLVM IR text size must stay well under the
    // exponential-blowup regime (see MEASURED / THRESHOLD above).
    static constexpr long kMaxIrLines = 150000;
    std::string ir = compileDiamondAndCaptureIR(src);
    BEZEL_EXPECT_TRUE(!ir.empty());
    long lineCount = std::count(ir.begin(), ir.end(), '\n');
    if (lineCount >= kMaxIrLines) {
        std::cout << "    #4234 STRUCTURAL REGRESSION: dumped IR was "
                  << lineCount << " lines (bound " << kMaxIrLines
                  << "; measured post-fix ~23,918, pre-fix ~1,704,231 at "
                     "this exact shape)\n";
    }
    BEZEL_EXPECT_TRUE(lineCount < kMaxIrLines);
}
