// Regression tests for #7804 -- rendering an INLINE ARRAY of STRINGS leaked
// the +1 its element held.
//
// THE DEFECT.
//
//   `var a: [2]string := ["x" + 1.ToString(), "y"];` binds a +1 PRODUCER
//   ("x1", the concatenation) into the array's first slot. The inline array
//   OWNS that reference: §15.1 (v0.353) makes an inline array's element
//   slots the owner's, and the compiler inserts the scope-end release that
//   discharges it (the `inlineArrayStringFields` Disposal).
//
//   That scope-end release is armed only for an inline-array local the
//   conservative escape walk proves NON-ESCAPING. The walk's member-access
//   rule (the #1747 record rule) says: "a member access on the tracked local
//   whose member RUNS USER CODE ... receives `this` as a pointer to the value
//   struct and may copy the whole record ... => a scope-end release would then
//   dangle that alias (UAF)". Applied to an INLINE ARRAY that rule is wrong,
//   because an inline array's member surface is FIXED and COMPILER-PROVIDED:
//   §26.2's "What can be extended" lists "Inline-array types (`extension [N]T`)"
//   among the types that CANNOT be extended, and §15.1.2 enumerates the whole
//   surface -- `arr.Length`, the indexer, `arr.ToArray()`, `arr.Contains(item)`.
//   No user code can run on an inline-array receiver, so the receiver's VALUE
//   copy cannot be stored anywhere that outlives the call.
//
//   =>=> So reading the array at all (`a.ToString()`, `a.Length`,
//   `a.ToArray()`, `a.Contains(x)`) marked the local ESCAPED, the scope-end
//   release was never armed, and the +1 its string element held was never
//   discharged. The SAME verdict came from the walk's CALL-ARGUMENT rule for
//   an inline array passed at a NON-RETAINING argument position
//   (`Console.WriteLine(a)`): a BUILT-IN callee has no resolved MethodSymbol,
//   so the walk's plain-parameter relaxation never fired and the argument fell
//   through to the conservative ESCAPE.
//
// MEASURED on the base tree (this lane's own worktree base `c3c33264`, the
// issue's own repro `g01.bzl`, `scripts/agent_check.sh --probe --asan`):
//
//     [2]string
//     [2]Cls?
//     [2]string [2]Cls?
//     ==================================================================
//     ==NNNN==ERROR: LeakSanitizer: detected memory leaks
//     Direct leak of 32 byte(s) in 1 object(s) allocated from:
//     Indirect leak of 3 byte(s) in 1 object(s) allocated from:
//     SUMMARY: AddressSanitizer: 35 byte(s) leaked in 2 allocation(s).
//
//   35 B in 2 allocations = the 32-byte `bz.string` header of the "x1"
//   element plus its 3-byte data buffer ("x1" is two code points at width 1).
//   The issue's own measurement -- "leaks 32+3 B once" -- is the same two
//   allocations.
//
// THE REPRO IS LOOPED (200x) HERE, AND THAT IS NOT COSMETIC. CLAUDE.md:
//   "A LEAK REPRO MUST BE LOOPED (50-500x) -- under LSan's default options a
//   one-shot repro is vacuous." MEASURED on the base tree: the ONE-SHOT
//   shape's leak is INVISIBLE to LSan's default scan -- the inline array's
//   slot is an `alloca` in `main`'s frame, so at exit the stale stack slot
//   still holds the pointer and LSan classifies the block as STILL REACHABLE
//   rather than lost. Re-running the one-shot program with
//   `LSAN_OPTIONS=use_stacks=0` makes the SAME 35 B / 2 allocations visible.
//   The looped shape puts each iteration's block out of reach, so the leak is
//   observed by LSan's default options -- which is what makes this a row and
//   not a report.
//
// ── THE LEAK ORACLE (this is the point of the file) ────────────────────────
// A row that asserted only the program's OUTPUT would be GREEN on the pre-fix
// compiler and would prove nothing. Every row here measures a COUNT: the
// number of allocations LeakSanitizer reports as leaked, read out of LSan's own
// "SUMMARY: AddressSanitizer: <bytes> byte(s) leaked in <allocations>
// allocation(s)." line via the harness's own parser
// (`bezel::test::aot_detail::parseLeakSummary`). The expected count is ZERO;
// on the pre-fix compiler the leaking rows read 400 allocations / 7000 bytes
// (or 398 / 6965 -- two blocks LSan saw as still-reachable). The rows also run
// under `runAotChecked`, whose LSan leg turns a leak into a failed row, so the
// count and the verdict agree.
//
// ── WHICH SPELLINGS LEAKED, AND WHICH DID NOT (all measured) ───────────────
// On the BASE tree, 200 iterations each:
//
//   `a.ToString()`          LEAKS  7000 B / 400 allocations   <- the issue's
//   `a.Length`              LEAKS  6965 B / 398 allocations      own leak
//   `a.ToArray()`           LEAKS  7000 B / 400 allocations
//   `a.Contains("y")`       LEAKS  7000 B / 400 allocations
//   `Console.WriteLine(a)`  LEAKS  6965 B / 398 allocations
//   `$"{a}"`                CLEAN  -- and `var s := $"{a}"` CLEAN
//   `var b: [2]string := a` CLEAN  (the copy)
//   `a[0]` (the indexer)    CLEAN
//   `[2]int` (no strings)   CLEAN
//
//   ⇒ The INTERPOLATION spelling does NOT leak, in the one-shot form or in the
//   looped form, on either LSan mode: `$"{a}"` reads the array's VALUE, not a
//   MEMBER of it, so the walk's member-access rule never fires on it and the
//   scope-end release was armed on the base tree too. The issue's repro
//   program renders the array BOTH ways (`a.ToString()` and `$"{a}"`), and its
//   single 35 B leak came from the `.ToString()` statement. The interpolation
//   row below is therefore a CONTROL -- and it is labelled as one.
//
// ── THE CONTROL ROWS ───────────────────────────────────────────────────────
// Every row labelled CONTROL passes on the PRE-FIX tree as well. Each says
// what it is a control FOR; none of them is the regression oracle.
//
// Spec: §15.1 (inline arrays), §15.1.2 (members and operations; the lifetime
// of inline arrays carrying borrows), §26.2 (what can be extended), §28.9
// (runtime-managed types), §8.5 (the default rendering of a value), stdlib
// §2.1 (the console), v0.353.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated -- the sanitizer report
// the AOT harness embeds in the diagnostic message when LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// THE ORACLE. `leakedAllocations` is the COUNT of allocations LeakSanitizer
// reports as leaked: 0 when there is no SUMMARY line (LSan prints one only
// when it finds leaks), and the summary's own number when there is one. The
// pre-fix compiler produced 400 (7000 B) for the `.ToString()` / `ToArray()` /
// `Contains` rows and 398 (6965 B) for the `Length` / `Console.WriteLine`
// rows; the fixed compiler produces 0.
//
// `leakedBytes` is the same measurement in bytes.
//
// The row also asserts that the program ran to completion with the expected
// output and that NO sanitizer report of any kind is present, so a double
// free (which ASan reports as "attempting double-free") fails the row too.
void expectNoLeak(const JitResult& r, const char* out) {
    // The row must NOT have skipped: a skipped row asserts nothing.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        diagnosticText(r), leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    // The COUNT, printed so a failing row's log carries the NUMBER the leak
    // oracle measured (the pre-fix compiler reads 400 allocations / 7000
    // bytes here, from the inline array's string element).
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    // No sanitizer report of any kind: a double free would land here.
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

// `line` repeated `n` times, each on its own line -- the expected stdout of a
// row that writes inside its 200-iteration loop.
std::string repeated(const char* line, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) {
        out += line;
        out += '\n';
    }
    return out;
}

// `block` repeated `n` times, verbatim -- the expected stdout of a row whose
// loop body writes SEVERAL lines. The block already carries its own
// terminators, so nothing is appended.
std::string repeatedBlock(const char* block, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += block;
    return out;
}

}  // namespace

// ===========================================================================
// 1/9 -- THE ISSUE'S OWN REPRO, looped.
//
// The issue's `g01.bzl`, verbatim, inside a 200-iteration loop: an inline
// array of strings with a +1 PRODUCER in its first slot, a companion inline
// array of a CLASS type (the issue's own control), and the two renderings.
//
// PRE-FIX (measured): the program leaks 400 allocations / 7000 B (the "x1"
//   element's string header + data buffer, once per iteration).
// POST-FIX: no leak, and the same three lines of output.
//
// The printed text is pinned too: §8.5's default for an inline array is the
// type's own name -- "[2]string" and "[2]M.Cls?" -- so a fix that changed the
// RENDERING (rather than the ownership) would be caught.
// ===========================================================================
BEZEL_TEST(inline_array_string_element_leak_7804,
           the_issues_own_repro_leaks_spec_15_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class Cls public constructor() end constructor; end Cls;
class P
  public static method Main(): int
    var i := 0;
    while i < 200 do
      var a: [2]string := ["x" + 1.ToString(), "y"];
      var n: [2]Cls? := [null, null];
      Console.WriteLine(a.ToString());
      Console.WriteLine(n.ToString());
      Console.WriteLine($"{a} {n}");
      i := i + 1;
    end;
    return 0;
  end Main;
end P;
)BZL"),
                 repeatedBlock("[2]string\n[2]M.Cls?\n[2]string [2]M.Cls?\n", 200)
                     .c_str());
}

// ===========================================================================
// 2/9 -- `a.ToString()`: THE LEAKING SPELLING THE ISSUE NAMES.
//
// The inline array of strings, with a +1 PRODUCER in its first slot, read
// through the `ToString()` member.
//
// PRE-FIX (measured): 400 allocations / 7000 B.
// POST-FIX: "[2]string" x200, no leak.
//
// The printed text is §8.5's default for the value -- the inline array's own
// type spelling -- and it is asserted so that a fix which changed the
// RENDERING (rather than the ownership) would be caught.
// ===========================================================================
BEZEL_TEST(inline_array_string_element_leak_7804,
           to_string_of_an_inline_array_of_strings_leaks_spec_15_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var i := 0;
    while i < 200 do
      var a: [2]string := ["x" + 1.ToString(), "y"];
      Console.WriteLine(a.ToString());
      i := i + 1;
    end;
    return 0;
  end Main;
end P;
)BZL"),
                 repeated("[2]string", 200).c_str());
}

// ===========================================================================
// 3/9 -- `a.Length`: §15.1.2's FIRST LISTED MEMBER.
//
// It reads the array and returns N -- it stores nothing and copies nothing --
// so it is the shortest distance between the over-conservative ESCAPE verdict
// and a leak.
//
// PRE-FIX (measured): 398 allocations / 6965 B.
// POST-FIX: "400" (200 iterations x 2), no leak.
// ===========================================================================
BEZEL_TEST(inline_array_string_element_leak_7804,
           length_of_an_inline_array_of_strings_leaks_spec_15_1_2) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var i := 0;
    var n := 0;
    while i < 200 do
      var a: [2]string := ["x" + 1.ToString(), "y"];
      n := n + a.Length;
      i := i + 1;
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end P;
)BZL"),
                 "400\n");
}

// ===========================================================================
// 4/9 -- `a.ToArray()`: THE MEMBER THAT COPIES THE ELEMENTS.
//
// §15.1.2: "`arr.ToArray(): owned Array<T>` -- a fresh heap array of the N
// slots: value-typed slots copied; class-typed slots stored as borrows." The
// fresh array RETAINS the string elements (the inline-array lowering calls
// `bz_array_retain_strings` on the result), so the fresh array co-owns a +1
// per slot and the local's scope-end release leaves it valid. This row is the
// one that would go RED on a double release of the string elements.
//
// PRE-FIX (measured): 400 allocations / 7000 B.
// POST-FIX: "done", no leak.
// ===========================================================================
BEZEL_TEST(inline_array_string_element_leak_7804,
           to_array_of_an_inline_array_of_strings_leaks_spec_15_1_2) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var i := 0;
    while i < 200 do
      var a: [2]string := ["x" + 1.ToString(), "y"];
      var t := a.ToArray();
      i := i + 1;
    end;
    Console.WriteLine("done");
    return 0;
  end Main;
end P;
)BZL"),
                 "done\n");
}

// ===========================================================================
// 5/9 -- `a.Contains(item)`: THE FOURTH LISTED MEMBER.
//
// §15.1.2: "`arr.Contains(item: T): bool` -- a linear search comparing `item`
// with each element by the element type's `Equals`". A member read like the
// others: the receiver's VALUE copy cannot outlive the call.
//
// PRE-FIX (measured): 400 allocations / 7000 B.
// POST-FIX: "200" (every iteration finds "y"), no leak.
// ===========================================================================
BEZEL_TEST(inline_array_string_element_leak_7804,
           contains_on_an_inline_array_of_strings_leaks_spec_15_1_2) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var i := 0;
    var n := 0;
    while i < 200 do
      var a: [2]string := ["x" + 1.ToString(), "y"];
      if a.Contains("y") then n := n + 1; end;
      i := i + 1;
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end P;
)BZL"),
                 "200\n");
}

// ===========================================================================
// 6/9 -- `Console.WriteLine(a)`: THE ARGUMENT-POSITION SHAPE.
//
// The array handed to the console's `value: Object` parameter (stdlib §2.1)
// instead of to a member of it. The console RENDERS the value and writes the
// text; it stores nothing, so the array's own elements are not retained by the
// callee. A BUILT-IN callee has no resolved MethodSymbol, so the walk's
// plain-parameter relaxation never fired on this argument either.
//
// PRE-FIX (measured): 398 allocations / 6965 B.
// POST-FIX: "[2]string" x200, no leak.
// ===========================================================================
BEZEL_TEST(inline_array_string_element_leak_7804,
           console_write_line_of_an_inline_array_of_strings_leaks_spec_2_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var i := 0;
    while i < 200 do
      var a: [2]string := ["x" + 1.ToString(), "y"];
      Console.WriteLine(a);
      i := i + 1;
    end;
    return 0;
  end Main;
end P;
)BZL"),
                 repeated("[2]string", 200).c_str());
}

// ===========================================================================
// 7/9 -- CONTROL: THE INTERPOLATION SPELLING.
//
// WHAT IT IS A CONTROL FOR: the issue names "$"{a}"" as one of the two
// renderings. MEASURED on the PRE-FIX tree, in the looped form and in the
// one-shot form and under both LSan modes: `$"{a}"` does NOT leak. It reads
// the array's VALUE, not a MEMBER of it, so the walk's member-access rule
// never fires on it and the scope-end release was armed on the base tree too.
// The issue's repro program renders the array BOTH ways and its single 35 B
// leak came from the `.ToString()` statement.
//
// This row therefore PASSES ON THE PRE-FIX TREE as well: it is a control, not
// the regression oracle. It pins that the fix did not change the
// interpolation's rendering ("[2]string", §8.5) and did not introduce a leak
// there.
//
// PRE-FIX and POST-FIX: "[2]string" x200, no leak.
// ===========================================================================
BEZEL_TEST(inline_array_string_element_leak_7804,
           interpolation_of_an_inline_array_of_strings_is_clean_spec_15_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var i := 0;
    while i < 200 do
      var a: [2]string := ["x" + 1.ToString(), "y"];
      Console.WriteLine($"{a}");
      i := i + 1;
    end;
    return 0;
  end Main;
end P;
)BZL"),
                 repeated("[2]string", 200).c_str());
}

// ===========================================================================
// 8/9 -- CONTROL: THE ARRAY IS NEVER READ.
//
// The same declaration with no member access and no argument position at all.
// The escape walk never marks the local ESCAPED, so the scope-end release IS
// armed on the base tree too -- this row pins that the fix did not change the
// already-clean shape.
//
// This row PASSES ON THE PRE-FIX TREE as well: it is a control, not the
// regression oracle.
//
// PRE-FIX and POST-FIX: "200", no leak.
// ===========================================================================
BEZEL_TEST(inline_array_string_element_leak_7804,
           an_unread_inline_array_of_strings_is_clean_spec_15_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var i := 0;
    var n := 0;
    while i < 200 do
      var a: [2]string := ["x" + 1.ToString(), "y"];
      n := n + 1;
      i := i + 1;
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end P;
)BZL"),
                 "200\n");
}

// ===========================================================================
// 9/9 -- CONTROL: AN ARRAY OF NON-STRINGS.
//
// The issue's own companion control (a `[2]Cls?`): an inline array whose
// elements carry no string has no `inlineArrayStringFields` release to arm, so
// there is nothing to leak. This row pins that the fix did not change it.
//
// This row PASSES ON THE PRE-FIX TREE as well: it is a control, not the
// regression oracle.
//
// PRE-FIX and POST-FIX: "[2]int" x200, no leak.
// ===========================================================================
BEZEL_TEST(inline_array_string_element_leak_7804,
           a_non_string_inline_array_is_clean_spec_15_1) {
    expectNoLeak(runAotChecked(R"BZL(
module M;
class P
  public static method Main(): int
    var i := 0;
    while i < 200 do
      var a: [2]int := [1, 2];
      Console.WriteLine(a.ToString());
      i := i + 1;
    end;
    return 0;
  end Main;
end P;
)BZL"),
                 repeated("[2]int", 200).c_str());
}
