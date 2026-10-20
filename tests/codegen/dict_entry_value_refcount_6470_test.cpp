// Codegen — `DictionaryEntry<K,V>.Value` for a STRING-V dictionary is read OUT
// as an OWNED reference (§28.11 *Collection element ownership*, stdlib §4.8,
// v0.338) that the caller must discharge, and the First/Last/Single/ElementAt
// terminal neither minted it nor let anyone discharge it.
//
// WHAT §28.11 SAYS, AND WHY THIS IS A DEFECT RATHER THAN A READING.
// §28.11's element-category table classifies a `string` slot as a shared
// reference ("runtime-managed reference (`string`, function types, `Type`) |
// shared references"), and its OPERATIONS table's "indexer-get / default
// iteration" row reads a shared-reference slot as "**reference out**" (only
// the class/interface and move-only columns read "borrow out"). stdlib §4.8's
// "`entry.Key` and `entry.Value` read the entry; for class-typed values,
// `entry.Value` is a borrow" scopes its borrow claim to CLASS-typed values, so
// it does not contradict the string case. The spec therefore answers this
// question, and no `spec-question` issue is warranted.
//
// MEASURED that the rest of the codebase already implements reference-out: a
// string read out of a dict into a local that OUTLIVES the dict's `do ... end`
// scope (`escape.bzl` in this lane's scratch) prints the right bytes at exit 0
// with no leak and no use-after-free — the indexer-get RETAINS. The terminal
// arm was the outlier.
//
// THE DEFECT, IN TWO HALVES THAT ARE ONLY CORRECT TOGETHER.
//
// Half 1 — the terminal handed the caller a BORROW. `decodeDictValueAt` passes
// a pointer-typed V through unchanged, and for this arm that pointer IS the
// receiver table's own `e.val` (the runtime's own header for
// `bz_dict_values_str`/`_gen`: "the values themselves are owned by the
// dictionary and must NOT be freed by the caller"). fix-forward #6419 had
// therefore to DECLINE `bz_dict_free`'s value discharge on this arm — freeing
// it read freed memory (MEASURED heap-use-after-free freed by
// `bz$strElemRelease` on dc3c8f2a). Correct given a borrow.
//
// Half 2 — declining the discharge strands the dict's +1 on EVERY entry of the
// column, not just the one that escapes, because `bz_dict_free`'s disposer is
// per-column and the whole column is declined together. MEASURED on this lane's
// parent over 101 iterations of the issue's own repro: `AddressSanitizer:
// 7272 byte(s) leaked in 404 allocation(s)` — exit 1, correct stdout `303`.
//
// Half 1 alone is a use-after-free; Half 2 alone is a leak. The fix takes the
// alternative both #6419 and the `Array<string>` sibling
// (`freeFreshInputTempUnlessStringElementEscapes`) recorded as NOT taken:
// the terminal MINTS the +1 (`emitStringRetain`, before the table free) so the
// returned entry's Value is the CALLER's own reference, the reclaim passes
// `dictValueColumnEscapes=false` so the table free discharges the column in
// full, and a new `dictEntryValueRelease` Disposal discharges the caller's +1
// at the entry local's scope end. The mint is keyed on a non-nullable `string`
// in generic slot 1 with the SAME conjuncts the reclaim uses to decide whether
// to install the value disposer, deliberately spelled identically so the two
// cannot drift — a mint without its discharge leaks, a discharge without its
// mint is a use-after-free.
//
// WHY A SCOPE-END DISPOSAL RATHER THAN TEACHING `isPlusOneStringProducer`.
// The #6419 comment predicted this fix would need `isPlusOneStringProducer` to
// learn these arms are +1 producers. It does not: that predicate governs
// whether a CONSUMER re-retains a produced string, and here the extra +1 is
// discharged by a dedicated scope-end Disposal on the ENTRY LOCAL — the shape
// #3720 already established for the entry's Key half, which is why the two
// halves now mirror each other field for field. The local is registered only
// when the existing `StringEscapeWalker` proves it never escapes, so nothing
// else holds the reference: no UAF, no double-free. An ESCAPING local is
// simply never registered, so its minted +1 leaks — §28.3's stated safe
// direction, and the identical trade the Key half has made since #3720.
//
// A leak pin must prove its detector ran, and WHICH HELPER THE ROW CALLS is
// what arms it — not an environment variable. The one-argument `runAotChecked`
// helper is `runAotCheckedImpl` called with the `/*detectLeaks=*/true` literal
// (tests/support/aot_support.hpp), and that flag is what builds
// `ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0`; the sibling
// `runAotCheckedLenient` passes `false` and runs `detect_leaks=0`, so a leak
// assertion written against IT is VACUOUS — green on a broken tree while
// printing the same clean "no leak" output an armed row prints. Every leak
// assertion below therefore goes through `runAotChecked`;
// `runAotCheckedLenient` is used only for the crash half (`expectRunsClean`),
// where `r.ran` is the oracle. A helper named `runAotChecked*` is NOT evidence
// the check is on: grep the `detectLeaks` argument in aot_support.hpp before
// asserting anything about a row's leak coverage.
//
// `BEZEL_AOT_DETECT_LEAKS` is not that knob. It is read only by
// `leakDetectMode()` (tests/support/force_sanitize.hpp), which `runAot` calls;
// `runAotCheckedImpl` never calls it, so the `LeakEnv("strict")` scopes in this
// file are INERT (see the note on the class). They are kept — and corrected
// rather than deleted — only so the rows read as they did; nothing about leak
// coverage depends on them.
//
// Every leak row asserts `!checkedToolchainSkipped(r)` BEFORE any guard that
// could absorb the skip — `if (skipped(r)) return;` inverts the oracle,
// reporting a clean pass on a box whose ASan link was unavailable. No
// `LSAN_OPTIONS` is set: these are found at LSan defaults.
//
// THE OTHER ARM IS NOT COVERED HERE, DELIBERATELY. `lowerDictEntryFor`'s
// loop-exit reclaim (LlvmBackend_Stmts.cpp) passes the same escape axis for a
// `for var e in <fresh dict>` whose loop variable a closure captures and may
// escape the loop — MEASURED still leaking 17,574 B / 1010 allocs over 101
// iterations after this fix. That arm has no plain-local scope end to hang a
// discharge on (the loop variable is a boxed cell), so closing it is a
// different mechanism, filed separately as #6473. The rows below pin the
// terminal arm only, and the non-escaping for-in control asserts the for-in
// arm's ALREADY-correct behaviour is untouched by this change.

#include <cstdlib>
#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

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

// The harness SKIPS (loudly, counted) when clang is absent; a row that
// asserts through that would be asserting on an unarmed oracle. A sanitizer
// finding is NOT a skip — "sanitizer fired" means it ran and found something.
bool skipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// 101 iterations — a one-shot leak repro is a vacuity trap: a single
// evaluation's table is small enough to hide under LSan noise, and the whole
// point of the defect is that it repeats once per evaluation. Every number
// quoted in this file is a 101-iteration measurement.
const char* kIterations = "i < 101";

// THE ISSUE'S REPRO'S PRODUCER. The two values are CONCATENATION producers
// (`"va" + "a"`, `"vb" + "b"`), each of which allocates its own +1 — so a leak
// on this shape is 4 allocations per iteration, which is exactly the 404 the
// parent measured over 101 iterations. A literal-valued producer would make the
// same leak smaller and easier to hide under LSan jitter.
std::string strValProducer() {
    return
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, string> "
        "    var d: Dictionary<string, string> := new Dictionary<string, string>(); "
        "    d.Add(\"aa\", \"va\" + \"a\"); d.Add(\"bb\", \"vb\" + \"b\"); "
        "    return d; "
        "  end MakeDict; "
        "end P; ";
}

// The int-keyed twin: same V, a generic K. §28.11's operations table reads a
// shared-reference slot as "reference out" regardless of the key type, so this
// must reach zero too — a fix that keyed only on a string key would leave it
// leaking, and `isFreshDictEntryKeyReleaseTerminalLocal` (the Key half's
// predicate) deliberately excludes it.
std::string intKeyStrValProducer() {
    return
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<int, string> "
        "    var d: Dictionary<int, string> := new Dictionary<int, string>(); "
        "    d.Add(7, \"va\" + \"a\"); d.Add(8, \"vb\" + \"b\"); "
        "    return d; "
        "  end MakeDict; "
        "end P; ";
}

// The BYTE-EXACT issue repro's consumer: a plain `var` entry local over an
// unbound `owned` receiver, read once, then dropped. `n` accumulates
// `.Value.Length` so the value is observably USED (a dead store could be
// optimized into a shape that never materializes the reference, which would
// make every row here a vacuous green).
std::string firstValueLengthLoop(std::string& producer) {
    return producer +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      var e := P.MakeDict().First(); "
        "      n := n + e.Value.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
}

// Bytes LSan reports for `program`, or 0 when it reports none. NOTE the trap
// this shape hides (and why the crash rows below are a separate oracle): a
// heap-use-after-free aborts the process BEFORE LSan prints its summary, so a
// UAF also reads as 0 here. A leak row alone therefore CANNOT prove the absence
// of a use-after-free — every leak row is paired with a crash-oracle row that
// runs the same program and asserts the program actually ran to completion.
//
// §30 and the sibling files' own `leaksAtLeast`: never pin an EXACT byte count
// as a CEILING, because LSan is conservative and its totals jitter. This fix's
// claim is stronger than a ceiling — it claims ZERO — and zero is the one
// bound LSan jitter cannot move in the failing direction (LSan under-reports
// leaked bytes only across a genuinely-live root set, which is not the regime
// here: every byte this fix strands was unreachable the moment the table free
// returned). So the assertion below is an exact `== 0`, not a ceiling.
long leakedBytes(const std::string& program) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(program);
    static const std::regex kLeak(R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak)) return std::stol(m[1]);
    }
    return 0;
}

// A CRASH-oracle row: the program must RUN TO COMPLETION, print `expected`,
// and exit 0. This is the oracle that catches a use-after-free or a
// double-free, which `leakedBytes` structurally cannot (a UAF aborts before
// LSan prints its summary, so it reads as 0 leaked). `r.ran` is the load-
// bearing assertion here: it is false if and only if the generated program
// died on a memory error.
void expectRunsClean(const std::string& program, const char* expected) {
    JitResult r = runAotCheckedLenient(program);
    // Loud-first, silent-second, in that order — never `if (skipped) return;`
    // before the assertion (see the file header and #3733).
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(expected) != std::string::npos);
}

// A LEAK-oracle row: strict LSan armed, and the program must run clean. Used
// for the arms this fix widens and for the controls it must NOT disturb.
void expectNoLeak(const std::string& program, const char* expected) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(expected) != std::string::npos);
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("byte(s) leaked") == std::string::npos);
    }
}
// ---------------------------------------------------------------------------

// THE ISSUE'S REPRO, ASSERTED AT ZERO — not at a ceiling. A ceiling passes at 0
// as readily as at 7,272, so it cannot distinguish "discharged" from "declined";
// this fix discharges, so the bound is 0. MEASURED on this lane's parent
// (e8411d87): 7272 B / 404 allocs, exit 1, correct stdout 303.
BEZEL_TEST(dict_entry_value_refcount_6470,
           first_on_a_string_valued_dictionary_leaks_nothing_spec_28_11) {
    std::string producer = strValProducer();
    BEZEL_EXPECT_EQ(leakedBytes(firstValueLengthLoop(producer)), 0);
    // The same program on the CRASH oracle — a leak row alone cannot prove the
    // absence of a use-after-free, because a UAF aborts before LSan prints its
    // summary and would read as 0 leaked. Run BOTH oracles on the issue's own
    // repro, not just the one that fails on the parent.
    expectRunsClean(firstValueLengthLoop(producer), "303");
}

// The int-keyed twin. Same V, generic K — §28.11's operations table reads a
// shared-reference slot as "reference out" regardless of K, so this reaches zero
// too. This is the row a fix keyed on a STRING KEY would miss: the Key half's
// predicate (`isFreshDictEntryKeyReleaseTerminalLocal`) excludes a non-string K
// deliberately, since a generic key is a plain by-value load with no heap
// ownership, while the Value half's key is V — not K.
BEZEL_TEST(dict_entry_value_refcount_6470,
           first_on_an_int_keyed_string_valued_dictionary_leaks_nothing_spec_28_11) {
    std::string producer = intKeyStrValProducer();
    BEZEL_EXPECT_EQ(leakedBytes(firstValueLengthLoop(producer)), 0);
    expectRunsClean(firstValueLengthLoop(producer), "303");
}

// THE OTHER THREE SPELLINGS of the same terminal, each over a fresh unbound
// receiver. One arm tested is a spelling test; the four are the actual surface.
// `isFreshDictEntryValueReleaseTerminalLocal` and the terminal arm's own
// `m == "First" || "Last" || "Single"` / `ElementAt` condition are spelled
// identically, so a spelling the predicate admitted but the arm did not lower —
// or the reverse — shows up here as exactly one of the three leaking.
//
// `ElementAt(0)` is the one-argument spelling and reads the same first entry as
// `.First()`. `Single()` is the odd one: per stdlib §4.8 it is the SINGLETON
// accessor and THROWS `InvalidOperationException` on a dictionary with more
// than one entry (MEASURED here: exit 70). That is the correct stdlib
// behaviour, not a defect, and asserting exit 0 over the 2-entry producer
// would be asserting against it. So `Single` gets a single-entry producer of
// its own — which also makes it the one arm whose leak cannot be masked by the
// second entry's value: a fix that discharged only the FIRST entry would show
// up as `Single` still leaking while `First`/`Last`/`ElementAt` went clean.
//
// Each reads `.Value.Length`, so the reference is observably used: a dead store
// could be optimized into a shape that never materializes it, making every row
// here a vacuous green.
BEZEL_TEST(dict_entry_value_refcount_6470,
           last_single_and_elementat_leak_nothing_too_spec_28_11) {
    // Two entries: `"va" + "a"` and `"vb" + "b"`, both length 3, so the
    // accumulator is 101 x 3 = 303 on every arm below.
    std::string producer = strValProducer();
    for (const char* arm : {"Last", "ElementAt"}) {
        std::string call = (std::string(arm) == "ElementAt")
                             ? ".ElementAt(0)" : ("." + std::string(arm) + "()");
        std::string program =
            producer +
            "class C public "
            "  static method Main(): int "
            "    var i := 0; "
            "    var n := 0; "
            "    while " + std::string(kIterations) + " do "
            "      var e := P.MakeDict()" + call + "; "
            "      n := n + e.Value.Length; "
            "      i := i + 1; "
            "    end while; "
            "    Console.WriteLine(n); "
            "    return 0; "
            "  end Main; "
            "end C; ";
        BEZEL_EXPECT_EQ(leakedBytes(program), 0);
        expectRunsClean(program, "303");
    }
    // `Single` over a SINGLE-entry dictionary: 101 x 3 = 303 as well.
    std::string singleProgram =
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, string> "
        "    var d: Dictionary<string, string> := new Dictionary<string, string>(); "
        "    d.Add(\"aa\", \"va\" + \"a\"); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      var e := P.MakeDict().Single(); "
        "      n := n + e.Value.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(singleProgram), 0);
    expectRunsClean(singleProgram, "303");
}

// AN ESCAPING ENTRY LOCAL LEAKS — and that is the SAFE direction, asserted so a
// future "fix" cannot close a row here by adding an UNCONDITIONAL release (which
// would be a use-after-free against an escaping consumer, or a double-free
// against one that took the reference). This local is retained past its own
// scope, so `StringEscapeWalker` excludes it from
// `dictEntryValueReleaseEligible_` and the minted +1 simply leaks — the
// identical trade `dictEntryKeyRelease` has made for the Key half since #3720.
//
// What this row DOES pin is the value: a caller that took `entry.Value` out and
// kept it still reads the right bytes after the entry goes out of scope. That
// is the observable the whole fix exists to produce (§28.11 "reference out"),
// and it is what the pre-fix borrow could not promise once the table was freed.
BEZEL_TEST(dict_entry_value_refcount_6470,
           an_escaping_entry_local_keeps_its_value_readable_spec_28_11) {
    std::string program =
        strValProducer() +
        "class C public "
        "  static method Main(): int "
        "    var s: string := \"\"; "
        "    do "
        "      var e := P.MakeDict().First(); "
        "      s := e.Value; "
        "    end do; "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    expectRunsClean(program, "vaa");
}

// THE CONTROL — the arm the fix must NOT have widened too far. A
// `Dictionary<string,int>` has NO co-owned +1 on its value column, so this leak
// was already zero before the fix and must stay zero: this is the row that
// catches the reclaim's discharge being installed where there was nothing to
// discharge (which would be a use-after-free on the entry's field 1, since the
// int was never heap-owned).
BEZEL_TEST(dict_entry_value_refcount_6470,
           first_on_a_string_keyed_int_valued_dictionary_still_leaks_nothing_spec_28_11) {
    std::string program =
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, int> "
        "    var d: Dictionary<string, int> := new Dictionary<string, int>(); "
        "    d.Add(\"aa\", 7); d.Add(\"bb\", 9); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      var e := P.MakeDict().First(); "
        "      n := n + e.Value; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    expectNoLeak(program, "7");
}

// THE FOR-IN CONTROL — the sibling arm this fix deliberately does NOT change.
// A `for var e in <fresh dict>` whose loop variable is NOT captured was already
// leak-free before this fix (its reclaim passes
// `dictValueColumnEscapes=false`, and the loop variable's field 1 is the
// table's own `e.val`, disposed by the table free once the loop is done). This
// row pins that the terminal arm's change did not disturb it, and it is the
// differential that says this fix is SCOPED: the for-in ESCAPING-capture arm is
// a different consumer shape (a boxed cell, not a plain local, so there is no
// scope end to hang a discharge on) and is filed separately as #6473.
BEZEL_TEST(dict_entry_value_refcount_6470,
           for_in_over_a_string_valued_dictionary_without_a_capture_still_leaks_nothing_spec_28_11) {
    std::string program =
        strValProducer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      for var e in P.MakeDict() do "
        "        n := n + e.Value.Length; "
        "      end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    expectNoLeak(program, "606");
}

}  // namespace
