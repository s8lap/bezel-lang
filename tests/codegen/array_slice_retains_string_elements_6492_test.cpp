// Ownership / disposal — a §15.6 range slice over an `Array<string>` co-owns a
// +1 per element.
//
// Authority:
//   §15.6 (Slicing), v0.338: "Slicing produces a new `Array<T>` containing
//   copies of the references (or values) in the range; mutating the slice does
//   not affect the original. The produced array is **owned** by the caller
//   (the slice is a fresh container, exactly like `ToArray()` …)".
//   §28.9 (Runtime-managed types), v0.338: every reference is released exactly
//   once.
//   §28.11 (Collection element ownership), v0.338: a `string` element takes
//   the shared-reference slot column; a borrowing collection's slots are the
//   whole element story, and its `Dispose()` row is "no user-visible work (the
//   backing storage is part of the instance's allocation, released with it)".
//
// THE DEFECT (#6492). `bz_array_slice` (src/runtime/bezel_runtime_collections.cpp)
// `memcpy`s the element POINTERS into the fresh `bz_array_t` and returns. It
// takes no `bz_array_retain_strings`, so the source array and the slice end up
// co-owning ONE `bz_string*` between them, and both disposers release it. The
// program's OUTPUT is already correct — the fault is in disposal, so it fires
// after the last correct value is printed, at teardown:
//
//   bezel: fatal: refcount over-release (string) — two releases for one
//   reference; terminating per §28.9        [exit 134, SIGABRT]
//
// Every other copy-materialiser in the compiler already performs this retain
// (`bz_lazy_to_array` at LlvmBackend_Calls3.cpp ~2880, the lazy-ctor snapshot at
// LlvmBackend_Exprs2.cpp ~7190), and §15.6 itself calls the slice "exactly like
// ToArray()". The slice arm was the one copy-materialiser that did not, so this
// is a one-line omission relative to a working sibling, not a family-wide gap.
//
// ⚠ THE REPRO TRAP, and it is load-bearing. This defect is INVISIBLE over plain
// string LITERALS: `a.Add("aa")` stores an immortal sentinel whose
// `bz_string_retain` is a no-op, so the over-release cannot drive the count
// negative and the program exits 0. Only RUNTIME-ALLOCATED strings — produced
// here by the INTERPOLATION `$"elem-{i}"` — expose the missing +1. Every row
// below that targets the defect uses the interpolation. The two literal/int
// rows are deliberately present as NON-REGRESSION controls and are expected to
// be green on the base too.
//
// LOOPED, NOT ONE-SHOT. A one-shot refcount fault is the classic vacuity trap
// (tests/README.md): the rows run 200 rounds, each building and slicing a fresh
// 3-element array, so a fix that merely shifted the double-release by one round
// — or one that traded the abort for a leak — is visible as 600 leaked strings
// rather than hidden under the sanitizer's noise.
//
// THE ORACLE. `runAotChecked` (the ASan/LSan oracle), never the deprecated
// in-process JIT harness: this is disposal work. It passes
// `/*detectLeaks=*/true` as a hardcoded argument (aot_support.hpp), so the
// harness arms LeakSanitizer BY CONSTRUCTION and a LEAK is a hard failure —
// which is what separates the two ways this can go wrong: the base ABORTS
// (over-release), and a fix that stops copying at all would instead produce the
// right count with a leak per element. Neither may pass.
//
// WHY `LeakEnv("strict")` IS STILL HERE even though it is not what makes these
// rows observe a leak. The `BEZEL_AOT_DETECT_LEAKS` knob is read at
// aot_support.hpp:3416 and :4014, which are `runAot` and the two-module path —
// NOT the `runAotChecked` family — so for these rows it is belt-and-braces
// rather than load-bearing. It is kept for the direction it DOES protect: the
// gate may run a whole shard with the knob set, and the RAII save/restore
// stops that from polluting a `runAot`-based row elsewhere in the same process.
// It is deliberately NOT what the leak rows rest on.
//
// The `expectClean` helper asserts `BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r))`
// so an unarmed oracle reads red rather than silently green.

#include <cstdlib>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "support/codegen_support.hpp"      // JitResult
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Save/restore BEZEL_AOT_DETECT_LEAKS around a row.
//
// IT IS INERT for every `runAotChecked` row in this file, and deliberately kept
// only so the rows read the same as they did before the header correction
// above. `runAotCheckedImpl` takes `detectLeaks` as a hardcoded argument and
// never consults this variable — `leakDetectMode()` is read only by `runAot` —
// so this scope changes nothing about the leak oracle. Do NOT read it as "this
// row's leak checking is armed here": it is `runAotChecked` vs
// `runAotCheckedLenient` that decides that. Delete it whenever the rows that
// carry it are next edited for another reason.
class LeakEnv {
public:
    explicit LeakEnv(const char* value) {
        const char* prev = std::getenv("BEZEL_AOT_DETECT_LEAKS");
        had_ = prev != nullptr;
        if (had_) prev_ = prev;
        if (value) ::setenv("BEZEL_AOT_DETECT_LEAKS", value, /*overwrite=*/1);
        else       ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    ~LeakEnv() {
        if (had_) ::setenv("BEZEL_AOT_DETECT_LEAKS", prev_.c_str(), 1);
        else      ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    LeakEnv(const LeakEnv&) = delete;
    LeakEnv& operator=(const LeakEnv&) = delete;
private:
    bool had_ = false;
    std::string prev_;
};

// The harness SKIPS (loudly, counted) when the clang toolchain is absent; a row
// that asserts through that would be asserting on an unarmed oracle.
bool skipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// Does any diagnostic carry the runtime's over-release headline? The runtime
// writes it to the program's stderr and aborts (bz_refcount_overrelease_fail,
// src/runtime/bezel_runtime_internal.hpp:105), and runAot surfaces a crashed
// program's stderr into the diagnostics under a "terminated by signal 6"
// headline — so on the base this needle IS reachable, and a fix that turned the
// abort into a DIFFERENT crash is distinguished from a clean run rather than
// passing on the exit code alone.
bool sawOverRelease(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("refcount over-release") != std::string::npos)
            return true;
    return false;
}

// The standard expectation: compiles, runs, prints exactly `wantStdout`,
// exits 0, and neither aborts nor leaks.
void expectClean(const std::string& module, const std::string& wantStdout) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(module);
    // Loud-first, silent-second — never `if (skipped) return;` before the
    // assertion (aot_result_asserts.hpp, #3733).
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_FALSE(sawOverRelease(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(wantStdout) != std::string::npos);
}

// THE PRODUCER — runtime-allocated strings, interpolated. §15.6's slice is
// `src[0..1]`, which keeps 2 of the 3 elements; the count is what the loop
// accumulates, so the value channel (stdout) pins BOTH that the slice took the
// right elements and that nothing aborted before the print.
//
// `Loop` wraps `Rounds` iterations; 200 rounds x 3 strings = 600 runtime
// strings, so a leaked-element regression is 600 allocations over the noise.
const char* kRounds = "200";

std::string indexRangeLoop(const char* rounds) {
    return "module M; "
           "class P "
           "  public static method MakeArr(): owned Array<string> "
           "    var a := new Array<string>(); "
           "    for var i in 0..<3 do a.Add($\"elem-{i}\"); end for; "
           "    return a; "
           "  end MakeArr; "
           "end P; "
           "class Program "
           "  public static method Main(): int "
           "    var total := 0; "
           "    for var round in 0..<" + std::string(rounds) + " do "
           "      var src: Array<string> := P.MakeArr(); "
           "      var s: Array<string> := src[0..1]; "
           "      total := total + s.Count; "
           "    end for; "
           "    Console.WriteLine(total); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// ===========================================================================
// (1) THE LOAD-BEARING ROW. The issue's own repro, looped 200x.
//
// MEASURED on the pre-fix tree (this lane's base, `fc131e13`):
//   print `2`, then
//     bezel: fatal: refcount over-release (string) — two releases for one
//     reference; terminating per §28.9        [exit 134, SIGABRT]
// so `r.ran` is false (SIGABRT is not a normal exit) and `sawOverRelease` is
// true. 200 rounds abort on the FIRST of them, which is why the loop is here
// for the LEAK axis rather than for the abort: with the retain in place every
// one of the 200 rounds' elements is co-owned and released exactly once.
// ===========================================================================
BEZEL_TEST(array_slice_retains_string_elements_6492,
           index_range_slice_over_runtime_strings_does_not_over_release) {
    // 200 rounds x 2 sliced elements = 400, printed on stdout (the 8-bit exit
    // channel cannot carry it and is not asked to).
    expectClean(indexRangeLoop(kRounds), "400");
}

// ===========================================================================
// (2) THE `Range`-TYPED-VARIABLE FORM (#1079's spelling). Sema types it
// through the same slice arm and codegen reaches the same
// `lowerArrayRangeSlice`, so it is the same defect by a different surface —
// MEASURED on the base: identical over-release abort, exit 134.
//
// Kept at one round: this row's job is to pin the SPELLING, not to add leak
// signal, and (1) already carries the loop. A spelling that stopped routing to
// the slice arm entirely would change `s.Count` from 2 to a diagnostic, which
// the stdout assertion catches.
// ===========================================================================
BEZEL_TEST(array_slice_retains_string_elements_6492,
           range_variable_slice_form_does_not_over_release) {
    expectClean(
        "module M; "
        "class P "
        "  public static method MakeArr(): owned Array<string> "
        "    var a := new Array<string>(); "
        "    for var i in 0..<3 do a.Add($\"elem-{i}\"); end for; "
        "    return a; "
        "  end MakeArr; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    var src: Array<string> := P.MakeArr(); "
        "    var r: Range<int> := 0..1; "
        "    var s: Array<string> := src[r]; "
        "    Console.WriteLine(s.Count); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "2");
}

// ===========================================================================
// (3) THE NAMED `.Slice(r)` METHOD FORM — stdlib §4.4, which §15.6 names "the
// compiler-recognized equivalent". It is the SAME `lowerArrayRangeSlice`, so
// the retain added there covers it; this row exists because a fix placed on
// the INDEXER arm alone (the narrower-looking edit) would leave this spelling
// un-retained and this row would go red.
//
// ⚠ THE BINDING SPELLING IS LOAD-BEARING HERE, AND IT IS NOT THE OBVIOUS ONE.
// Measured on the BASE, over 200 rounds, `BEZEL_AOT_DETECT_LEAKS=strict` (that
// knob is INERT for the rows below — see the file header; the rows are armed
// by the helper they call):
//   `using var s: Array<string> := src.Slice(0..1);`
//       -> bezel: fatal: refcount over-release (string)   [abort]   DETERMINISTIC
//   `var s: Array<string> := src.Slice(0..1);`  (and `var s := …`)
//       -> 14,400 bytes in 400 allocations leaked — the SLICE CONTAINER, from
//          `bz_array_slice`'s `bz_array_alloc` (stack #1
//          bezel_runtime_collections.cpp:556), never freed at all.
// The second spelling never reaches the element release, so it cannot show the
// over-release; it is a DIFFERENT, pre-existing defect (the `.Slice()` result
// under a plain `var` binding is never reclaimed — filed separately), and it is
// deliberately NOT what this row exercises. `using var` is the fully-discharged
// spelling, and it makes the row a deterministic abort pin on the base that is
// clean with the fix.
// ===========================================================================
BEZEL_TEST(array_slice_retains_string_elements_6492,
           named_slice_method_form_does_not_over_release) {
    expectClean(
        "module M; "
        "class P "
        "  public static method MakeArr(): owned Array<string> "
        "    var a := new Array<string>(); "
        "    for var i in 0..<3 do a.Add($\"elem-{i}\"); end for; "
        "    return a; "
        "  end MakeArr; "
        "end P; "
        "class Program "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var round in 0..<" + std::string(kRounds) + " do "
        "      var src: Array<string> := P.MakeArr(); "
        "      using var s: Array<string> := src.Slice(0..1); "
        "      total := total + s.Count; "
        "    end for; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "400");
}

// ===========================================================================
// (4) §15.6's OTHER HALF — "mutating the slice does not affect the original".
// The indexer-set on a slice DISPOSES the element it replaces (§28.11's store
// row: "indexer-set disposes the previous value first"). Over a slice whose
// element slots carried no +1 of their own, that dispose released the SOURCE's
// reference, so the source's string was freed while the source still held it.
//
// MEASURED on the base: prints `mutated`, then
//   `Segmentation fault (core dumped)`          [exit 139, SIGSEGV]
// on the following `Console.WriteLine(src[1])`. With the retain, the slice
// owns its own +1, the indexer-set releases THAT one, and `src[1]` still reads
// `elem-1`. Both prints are asserted, so a fix that suppressed the segfault by
// making the store not write would fail on `mutated` rather than pass.
// ===========================================================================
BEZEL_TEST(array_slice_retains_string_elements_6492,
           mutating_the_slice_does_not_affect_the_original) {
    expectClean(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    var src := new Array<string>(); "
        "    for var i in 0..<4 do src.Add($\"elem-{i}\"); end for; "
        "    var s: Array<string> := src[1..2]; "
        "    s[0] := \"mutated\"; "
        "    Console.WriteLine(s[0]); "
        "    Console.WriteLine(src[1]); "
        "    Console.WriteLine(src.Count); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "elem-1");
}

// ===========================================================================
// (5) NON-REGRESSION — an `Array<int>` slice. `int` has no per-element
// disposer, so there is nothing to over-release and this shape was NEVER
// affected; the retain must stay inert for it. MEASURED: the pre-fix and
// post-fix binaries are BYTE-IDENTICAL for this program (sha256
// 95aa7c1c4a05ff4ad644a0f7a5e119c8a0d505c1fe686a5a613ee82b72737a97 on both
// legs) — the mechanical proof that the change adds nothing for a non-string
// element rather than merely happening to be harmless at run time.
//
// The same byte-identity was measured for a CLASS-element `Array<Box>` slice
// (sha256 1143de245eac3f32d390eef9b56dadcd2fc6fb802f4ceb6fe77afeca393ff928 on
// both legs), which is the sharper half of the claim: §15.6's "disposing the
// slice never disposes elements" is stated for class-typed `T`, so a retain
// that could fire on a class element would be a §15.6 violation rather than a
// fix. `emitArrayRetainStringsIfStringArray` requires the element to be a
// non-nullable `string` PRIMITIVE in the RuntimeManaged category, so a class
// element is structurally excluded — and the identical binary is the proof
// that the exclusion holds in the emitted code, not just in the predicate.
// ===========================================================================
BEZEL_TEST(array_slice_retains_string_elements_6492,
           int_array_slice_is_unaffected_and_still_slices) {
    expectClean(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    var src := new Array<int>(); "
        "    for var i in 0..<5 do src.Add(i * 11); end for; "
        "    var s: Array<int> := src[1..3]; "
        "    Console.WriteLine(s.Count); "
        "    Console.WriteLine(s[0]); "
        "    Console.WriteLine(s[1]); "
        "    Console.WriteLine(s[2]); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "22");
}

// ===========================================================================
// (6) NON-REGRESSION — an `Array<string>` built from plain LITERALS. This is
// the trap the issue warns about: `bz_string_retain` is a no-op on a sentinel,
// so the base exits 0 here and this row is GREEN on both legs by construction.
// It is present precisely BECAUSE it cannot fail on the defect: a fix that
// over-retained (a second +1 per element) would show here as a leak of 600
// immortal-slot references under LSan, which is the mirror of the defect and
// the direction `expectClean`'s `LeakEnv("strict")` also guards.
// ===========================================================================
BEZEL_TEST(array_slice_retains_string_elements_6492,
           literal_string_array_slice_is_unaffected_and_still_slices) {
    expectClean(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    var src := new Array<string>(); "
        "    src.Add(\"aa\"); src.Add(\"bb\"); src.Add(\"cc\"); "
        "    var s: Array<string> := src[0..1]; "
        "    Console.WriteLine(s.Count); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "2");
}
