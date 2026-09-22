// fix #4546 (stdlib §4.17 "SortedDictionary<K, V>" / spec §28.6 "Leak
// prevention" / §28.9 "Runtime-managed types", v0.306): `SortedDictionary<
// string, V>.FirstKey` / `.LastKey` leaked the `bz_string` they mint on
// EVERY read, unbounded in a loop.
//
// TWO codegen sites build the identical fresh heap `bz_string` from the
// runtime's malloc'd key-copy bytes and call `initStringRc` (rc := 1), and
// NEITHER discharged that +1:
//   * LlvmBackend_Exprs.cpp  — the PROPERTY-syntax bare MemberAccessExpr
//     (`d.FirstKey`, no parens; `isSortedDictMember` arm).
//   * LlvmBackend_Calls.cpp  — the CALL-syntax CallExpr (`d.FirstKey()`,
//     `tryLowerCollectionInstanceCall`'s `isDict` block, the
//     `bz_dict_min_key_str` / `bz_dict_max_key_str` arm).
// Both are real, reachable spellings (stdlib §4.17 documents the property
// form; #1426/#3865's own tests exercise the call form identically).
//
// §28.6 holds an accessor read face to lend-only ("a getter may not return a
// fresh owned value"); stdlib §4.17 types both properties as `K`, and for
// `K = string` — a runtime-managed type, §28.9 — the caller-side discharge is
// the ordinary +1-producer contract every other string mint in this codegen
// already uses (`isPlusOneStringProducer` / `emitStringRelease`, the #3703 /
// #429 mechanism). THE FIX recognizes both codegen shapes as +1 producers in
// `Emitter::isPlusOneStringProducer` (LlvmBackend_Setup.cpp), gated on the
// receiver being `SortedDictionary<K, V>` with K a non-nullable `string` —
// routing BOTH sites through the single existing discipline rather than
// patching each release point individually.
//
// MEASURED (this fix's own FAIL-WITHOUT, `scripts/agent_check.sh --probe
// --asan`, 50-iteration loop reading both properties): pre-fix, BOTH the
// property-syntax and call-syntax probes leaked identically —
// "3500 byte(s) leaked in 200 allocation(s)" — rooted at `bz_key_dup` via
// `bz_dict_min_key_str` / `bz_dict_max_key_str` (the same runtime entry
// points #4499 gave the key's §4.6 storage width; #4546 is pre-existing on
// both sides of #4499, unrelated to that fix). Post-fix, both probes are
// LSan-clean with the identical output value — confirming the fix removes
// the leak WITHOUT freeing the string before its use (a premature release
// would read as a wrong value or an ASan use-after-free, not merely a leak).
//
// ORACLE CHOICE. A ONE-SHOT LSan read is documented (CLAUDE.md's vacuous-
// oracle catalogue) to pass on BOTH sides of a fix like this one — a single
// allocation can stay reachable from a live stack slot/register. Every leak
// row below loops (100-200 iterations, well within the 50-500 band) and
// combines the read with an accumulator so the loop cannot be constant-
// folded away. `runAotChecked` (tests/support/aot_support.hpp) sets
// `sanitizeAddress` on the GENERATED program and forks a separate native
// exec per run — the SAME `Driver::compileFile` + `--sanitize=address`
// mechanism this fix's own FAIL-WITHOUT probe used and confirmed non-
// vacuous in a lane worktree (a real ASan/LSan report was observed here).
// This is explicitly NOT the per-file `runUnderAsan`/`asanRuntimeObject()`
// family (tests/support/run_under_asan.hpp), which IS documented vacuous in
// a lane worktree (`asanRuntimeObject()` returns empty, short-circuiting the
// leg to a silent pass) — `runAotChecked` only gates on a `clang --version`
// toolchain probe, which this lane's own build already proves present.
//
// Separately, value-correctness rows below (via `runAotOut`, no ASan) assert
// the ACTUAL returned key text, not just that ASan stayed quiet — so a
// hypothetical over-eager release (freeing the string before its read) would
// surface as a wrong value or crash, not a false-clean leak row.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// LEAK oracle: any sanitizer finding (leak, UAF, double-free) at exit flips
// `ran` to false, so a leaking or corrupting program reports -999 here; a
// clean program reports its real exit code.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

std::string wrap(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           "end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// Leak-fixed rows — loop-amplified (150x), PROPERTY syntax (`d.FirstKey`,
// LlvmBackend_Exprs.cpp). Each iteration reads BOTH properties and folds
// their lengths into an accumulator so the loop cannot be optimized away.
// Two 1-char keys -> 2 per iteration -> 300 over 150 iterations.
//
// The accumulator is read back via STDOUT, not the process exit code:
// `runAot`'s `exitCode` is WEXITSTATUS-masked to 8 bits (CLAUDE.md's
// vacuous-oracle catalogue — 300 would read back as 44), so asserting it
// directly would silently pass on a truncated-but-wrong value. `r.ran`
// alone is the ASan/LSan leak/UAF/double-free oracle; `r.stdoutText` is the
// untruncated value oracle.
// ---------------------------------------------------------------------------
BEZEL_TEST(sorted_dict_firstkey_lastkey_leak_4546,
           property_syntax_loop_no_leak_stdlib_4_17) {
    JitResult r = runAotChecked(wrap(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"a\"] := 1; d[\"z\"] := 2; "
        "  var n := 0; "
        "  var i := 0; "
        "  while i < 150 do "
        "    var f := d.FirstKey; var l := d.LastKey; "
        "    n := n + f.Length + l.Length; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine($\"n={n}\"); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);  // neither leaked, double-freed, nor use-after-freed
    BEZEL_EXPECT_EQ(r.stdoutText, "n=300\n");
}

// ---------------------------------------------------------------------------
// Leak-fixed rows — loop-amplified (150x), CALL syntax (`d.FirstKey()`,
// LlvmBackend_Calls.cpp's tryLowerCollectionInstanceCall). The issue names
// this as a SEPARATE mint site from the property-syntax one above; this row
// proves it independently rather than assuming the two share a fate.
// ---------------------------------------------------------------------------
BEZEL_TEST(sorted_dict_firstkey_lastkey_leak_4546,
           call_syntax_loop_no_leak_stdlib_4_17) {
    JitResult r = runAotChecked(wrap(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"a\"] := 1; d[\"z\"] := 2; "
        "  var n := 0; "
        "  var i := 0; "
        "  while i < 150 do "
        "    var f := d.FirstKey(); var l := d.LastKey(); "
        "    n := n + f.Length + l.Length; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine($\"n={n}\"); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "n=300\n");
}

// ---------------------------------------------------------------------------
// Value-correctness rows (no ASan): the actual key TEXT must still read back
// correctly, not merely "no leak". A premature release (freed before use)
// would surface here as a wrong string, garbage, or a crash — a byte-count-
// only oracle cannot distinguish "released correctly at scope end" from
// "released too early and got lucky on this allocator".
// ---------------------------------------------------------------------------
BEZEL_TEST(sorted_dict_firstkey_lastkey_leak_4546,
           property_syntax_value_correct_stdlib_4_17) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "  using var d := new SortedDictionary<string,int>(); "
            "  d[\"banana\"] := 2; d[\"apple\"] := 1; d[\"cherry\"] := 3; "
            "  var f := d.FirstKey; var l := d.LastKey; "
            "  Console.WriteLine($\"f={f} l={l}\"); "
            "  return 0; ")),
        "f=apple l=cherry");
}

BEZEL_TEST(sorted_dict_firstkey_lastkey_leak_4546,
           call_syntax_value_correct_stdlib_4_17) {
    BEZEL_EXPECT_EQ(
        runAotOut(wrap(
            "  using var d := new SortedDictionary<string,int>(); "
            "  d[\"banana\"] := 2; d[\"apple\"] := 1; d[\"cherry\"] := 3; "
            "  var f := d.FirstKey(); var l := d.LastKey(); "
            "  Console.WriteLine($\"f={f} l={l}\"); "
            "  return 0; ")),
        "f=apple l=cherry");
}

// Re-reading each property TWICE per iteration (loop-amplified, 100x): each
// read mints its OWN independent +1 (they are not the same heap object), so
// this also guards against a fix that accidentally shares/aliases the two
// reads instead of releasing each one's own mint. Stdout again, not the exit
// code (800 would truncate to 32 through WEXITSTATUS).
BEZEL_TEST(sorted_dict_firstkey_lastkey_leak_4546,
           repeated_reads_independent_no_leak_stdlib_4_17) {
    JitResult r = runAotChecked(wrap(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"ab\"] := 1; d[\"cd\"] := 2; "
        "  var n := 0; "
        "  var i := 0; "
        "  while i < 100 do "
        "    n := n + d.FirstKey.Length + d.FirstKey.Length + "
        "            d.LastKey.Length + d.LastKey.Length; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine($\"n={n}\"); "
        "  return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "n=800\n");
}

// Direct (non-local-bound) consumption — the value flows straight into the
// concatenation/WriteLine argument position rather than through a named
// `var`, exercising the OTHER `isPlusOneStringProducer`-gated consumer sites
// (not just the local-decl bind/release path the rows above hit).
BEZEL_TEST(sorted_dict_firstkey_lastkey_leak_4546,
           direct_writeline_argument_loop_no_leak_stdlib_4_17) {
    BEZEL_EXPECT_EQ(runChecked(wrap(
        "  using var d := new SortedDictionary<string,int>(); "
        "  d[\"ab\"] := 1; d[\"cd\"] := 2; "
        "  var i := 0; "
        "  while i < 150 do "
        "    Console.WriteLine(d.FirstKey + d.LastKey); "
        "    i := i + 1; "
        "  end; "
        "  return 0; ")),
        0);
}
