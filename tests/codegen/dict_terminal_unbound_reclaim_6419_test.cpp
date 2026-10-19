// Codegen — the dict-backed `Dictionary<K,V>` member arms of the `isDict` block
// do not reclaim an UNBOUND `owned` receiver, the shared reclaim helper
// declines a string-KEYED one outright, and it never cascades the §28.11 value
// column. §28.3 (Unbound owned temporaries), §28.9, §28.11, stdlib §4.8 /
// §4.9 / §4.17, v0.338.
//
// THE DEFECT, IN THREE HALVES, AND EACH ONE IS NEEDED FOR A DIFFERENT REPRO.
//
// Half 1 — PLACEMENT. `Emitter::tryLowerCollectionInstanceCall`'s `isDict`
// block carried `freeUnboundOwnedBorrowingArrayTemp` in only its `.Count` arm
// and its First/Last snapshot arm. `.Contains`/`.ContainsKey`, `.Add`,
// `.TryAdd`, `.Remove` and `.Clear` reached NO discharge at all, so an
// `owned Dictionary<K,V>`-producing call consumed only in receiver position
// (`f.Make().Contains(k)`) leaked the whole table — header, bucket array,
// insertion-order spine and every key copy — once per evaluation. This is the
// same shape #6394 fixed one block over, in `isArr`'s Set arms; the two blocks
// are separate `if`s in the same function and only `isArr`'s was given the
// reclaim.
//
// Half 2 — ADMISSION. `freeUnboundOwnedBorrowingArrayTemp` itself returned
// early for a string-KEYED dict head, BEFORE reaching its `bz_dict_free`
// emission. That early return declined the per-key string RELEASE, but it
// declined the WHOLE free with it, so even the arms that already called the
// helper (`.Count`, since #1013) leaked. Half 2 is therefore invisible to a
// test written only against Half 1's arms, and Half 1 is invisible to a test
// written only against a string key — `.Count` is the row that isolates it.
//
// WHAT THE SPEC SAYS, AND WHY BOTH HALVES ARE DEFECTS RATHER THAN CHOICES.
// §28.3 (*Unbound owned temporaries*): a temporary "is disposed at the end of
// the full expression in which it is produced, after every use of it within
// that expression has completed" — so what differs per arm is PLACEMENT, not
// the trigger. And on why a name-keyed list is the wrong remedy: "The `owned`
// return qualifier is the single signal that drives the reclaim; there is no
// special-cased list of method names." Every row below therefore calls the SAME
// helper on the SAME population, placed after the operation that consumes the
// receiver.
//
// Half 2's early return was justified by a real absence — no release contract
// exists for a dict's string keys — but the absence is of a RELEASE, and it
// was applied to the BACKING. Reading the two runtime entry points settles it
// (src/runtime/bezel_runtime_collections.cpp): `bz_dict_set_str` stores
// `bz_key_dup(...)` and `bz_dict_set_gen` stores `malloc(key_size)` +
// `memcpy` — a BYTE COPY in both modes — so a dict's KEY column holds no
// co-owned +1 on any `bz_string*`, and `bz_dict_free(d, NULL)` reclaims the
// key copies, the bucket array, the order spine and the header completely.
// Nothing needed releasing, so declining cost the whole container.
//
// Half 3 — THE VALUE COLUMN. Even once the table is freed, a borrowing
// `Dictionary<K, string>` co-owns a +1 per stored VALUE (§28.11), and
// `freeUnboundOwnedBorrowingArrayTemp` derived its disposer from `genericArgs[0]`
// — for a Dictionary that is the KEY — so it reached `bz_dict_free(d, NULL)` and
// stranded one +1 per entry. Every OTHER release site in the backend already
// cascaded this column: `.Remove` (`bz_dict_remove_str_disposing` +
// `stringElementReleaseThunk`), `.Clear` (`bz_dict_clear` + the same thunk), the
// indexer overwrite, `registerUsingDisposal`'s dictionary branch, and
// `buildCollectionTempDisposal`. The unbound-temporary path was the one that did
// not, which is what made Half 2's admission unsafe to withdraw on its own — it
// would have freed the table of a `Dictionary<string,string>` and left its
// values behind. The three halves are one fix because each is a precondition of
// the next being correct.
//
// PRE-FIX (101 iterations, `bezelc --sanitize=address` on this fix's parent
// 867dfb00), each against the byte-identical BOUND twin measuring 0:
//
//   string-keyed `Dictionary<string,int>`          int-keyed `Dictionary<int,int>`
//   MakeDict().Contains("bb")   42,925 B /  606    .Contains(2)     43,228 B / 606
//   MakeDict().ContainsKey("bb")42,925 B /  606    .Add(9, 9)       43,632 B / 707
//   MakeDict().Add("dd", 4)     43,228 B /  707    .Remove(2)       42,824 B / 505
//   MakeDict().TryAdd("dd", 4)  43,228 B /  707    .Clear()         42,016 B / 303
//   MakeDict().Remove("aa")     42,622 B /  505    .Count           0  (control)
//   MakeDict().Clear()          42,016 B /  303
//   MakeDict().Count            42,925 B /  606
//
// The two `.Count` rows are the diagnostic pair: the string-keyed one leaks at
// the SAME 42,925 B as the arms that never called the helper at all, and the
// int-keyed one is clean — because that arm has carried the call since #1013
// and the helper admits an int key. Same arm, same placement, one fact apart.
//
// WHAT IS DELIBERATELY NOT CLAIMED HERE. The dict VALUE-returning arms —
// `.TryGetValue`, `.GetOrNull`, `.GetOrAdd`, `.PopValue`, `.TryPopValue`, the
// indexer read, `.Keys` / `.Values` — still reach no reclaim, and are still
// measurable: a `Dictionary<string,int>` `.GetOrNull` and a `TryGetValue` each
// measure 42,925 B / 606 allocs on this fix's parent. They are excluded on the
// same ground #6394 excluded its element-returning siblings: what the arm
// returns is a value read OUT of the table, so whether a reclaim is safe there
// depends on whether that value aliases a slot of the backing — a question the
// §28.11 string-VALUE row below shows cannot be answered by the value column's
// type alone. Placing one there needs its own exactly-once proof and is a
// separate change.
//
// AND WHAT HALF 3 ITSELF IS SUBJECT TO (fix-forward #6419, the LAST section of
// this file). That paragraph's exactly-once proof counts DISCHARGES against the
// table's own +1s. It does not cover a reference the call site has already
// handed to the caller OUT of one of those same slots — and `.First`/`.Last`/
// `.Single`/`.ElementAt` do exactly that, because `decodeDictValueAt` returns a
// pointer-typed V as "a borrow" (the table's own `e.val`), as does the dict
// for-in loop's loop variable. On those arms the cascade is a heap-use-after-
// free, not a leak. So the arm list above is right and the REASONING was not
// complete: the fix-forward adds the escape axis, declines the value discharge
// at the two sites that hand a slot out, and keeps the table free everywhere.
//
// A leak pin must prove its detector ran. `runAotChecked` defaults to
// `ASAN_OPTIONS=detect_leaks=0`, so a plain row is VACUOUS for a leak: green on
// a broken tree while showing a clean sanitizer trio. Every leak row below arms
// the leak oracle with `BEZEL_AOT_DETECT_LEAKS=strict` (save/restored, so a gate
// running the knob does not leak into a sibling row) and asserts
// `!checkedToolchainSkipped(r)` BEFORE any guard that could absorb the skip —
// `if (wasSkipped(r)) return;` inverts the oracle, reporting a clean pass on a
// box whose ASan link was unavailable. No `LSAN_OPTIONS` is set: these are
// found at LSan defaults.

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

// Save/restore BEZEL_AOT_DETECT_LEAKS around a row — see the file header.
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
// quoted in the file header is a 101-iteration measurement.
const char* kIterations = "i < 101";

// The producer module for a string-keyed `Dictionary<string,int>`.
std::string strDictProducer() {
    return
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, int> "
        "    var d: Dictionary<string, int> := new Dictionary<string, int>(); "
        "    d.Add(\"aa\", 1); d.Add(\"bb\", 2); d.Add(\"cc\", 3); "
        "    return d; "
        "  end MakeDict; "
        "end P; ";
}

// The producer module for a GENERIC-keyed `Dictionary<int,int>` — the half of
// the diagnosis the helper already admitted.
std::string genDictProducer() {
    return
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<int, int> "
        "    var d: Dictionary<int, int> := new Dictionary<int, int>(); "
        "    d.Add(1, 1); d.Add(2, 2); d.Add(3, 3); "
        "    return d; "
        "  end MakeDict; "
        "end P; ";
}

// One leak row over a string-keyed dictionary. `acc` counts a real observation
// of the arm's RESULT, so the row asserts the arm ran and returned the right
// thing as well as not leaking — a reclaim placed so early that it freed the
// table before the read would fail here too, and not merely "stop leaking".
// Pass `expectedAcc < 0` for a `unit`-returning arm, where the only observable
// is that the program ran to completion.
void expectStrKeyArmNoLeak(const char* armBody, int expectedAcc) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(
        strDictProducer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + kIterations + " do "
        "      " + armBody + " "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    // #3733: a silent `return` here would report a clean PASS on a box whose
    // ASan link is unavailable — green while asserting nothing about memory
    // safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    if (expectedAcc >= 0) {
        BEZEL_EXPECT_TRUE(r.stdoutText.find(std::to_string(expectedAcc)) !=
                          std::string::npos);
    }
}

// The generic-keyed twin, over the same loop shape.
void expectGenKeyArmNoLeak(const char* armBody, int expectedAcc) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(
        genDictProducer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + kIterations + " do "
        "      " + armBody + " "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    if (expectedAcc >= 0) {
        BEZEL_EXPECT_TRUE(r.stdoutText.find(std::to_string(expectedAcc)) !=
                          std::string::npos);
    }
}

// ===========================================================================
// THE ESCAPE AXIS (fix-forward #6419) — helpers.
//
// `r.ran == false` is AMBIGUOUS for these rows: a LeakSanitizer finding and a
// heap-use-after-free both stop the process with a non-zero status, and these
// arms legitimately leak the values they decline to discharge. So the oracle
// here is NOT `r.ran` — it is (a) a crash-oracle row with LeakSanitizer OFF,
// where `r.ran` IS unambiguous, and (b) a separate magnitude row for the leak.
// Folding the two into one `runAotChecked` row would let the leak mask the
// crash it is standing next to.

// A crash row: LeakSanitizer OFF (`runAotCheckedLenient` — the same oracle
// `dict_entry_for_var_capture_escaping_release_4161_test.cpp` uses, for the
// same reason: the path deliberately leaves a runtime-managed value leaking,
// never trading a leak for a use-after-free, so a strict LSan oracle would
// flag the accepted trade-off as a regression). Here `r.ran` is unambiguous —
// it is false if and only if the generated program died on a memory error.
void expectNoMemoryError(const std::string& program, const char* expected) {
    JitResult r = runAotCheckedLenient(program);
    // Loud-first, silent-second, in that order — never `if (skipped) return;`
    // before the assertion (see the file header).
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(expected) != std::string::npos);
}

// A no-leak row (the shape that is NOT an escape): strict LSan armed.
void expectNoLeak(const std::string& program, const char* expected) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(expected) != std::string::npos);
}

// Total bytes LSan reports as leaked, or 0 when it reports no leak at all.
// §30 (and the sibling files' own `leaksAtLeast`): never pin an EXACT byte
// count — LSan is conservative and its totals jitter. The caller asserts a
// generous CEILING, whose job is to separate two regimes that differ by ~6x,
// not to pin a number.
long leakedBytes(const std::string& program) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(program);
    static const std::regex kLeak(R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak))
            return std::stol(m[1].str());
    }
    return 0;
}

}  // namespace

// ===========================================================================
// HALF 1, STRING-KEYED: the arms that reached no discharge at all. Every one
// measured 42-43 KB on the parent (header + buckets + key copies + order
// spine, per evaluation).
// ===========================================================================
BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           contains_on_an_unbound_owned_dictionary_is_reclaimed_spec_28_3) {
    expectStrKeyArmNoLeak(
        "if P.MakeDict().Contains(\"bb\") then n := n + 1; end if;", 101);
}

BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           contains_key_on_an_unbound_owned_dictionary_is_reclaimed_spec_28_3) {
    expectStrKeyArmNoLeak(
        "if P.MakeDict().ContainsKey(\"bb\") then n := n + 1; end if;", 101);
}

BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           add_on_an_unbound_owned_dictionary_is_reclaimed_spec_28_3) {
    // `.Add` returns unit, so the row's only observable is that the arm ran.
    // Its reclaim's PLACEMENT — after `bz_dict_set_str`, so the table owns the
    // inserted entry when it is freed — is what the leak oracle and the
    // `SortedDictionary` and bound-control rows below jointly pin.
    expectStrKeyArmNoLeak("P.MakeDict().Add(\"dd\", 4);", -1);
}

BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           try_add_on_an_unbound_owned_dictionary_is_reclaimed_spec_28_3) {
    // `TryAdd`'s reclaim belongs at `endBB`, the join of the duplicate edge
    // (value disposed, table untouched) and the insert edge (value transferred).
    // A reclaim placed in the insert block would free the table on one path
    // only; this row runs 101 evaluations of a table whose keys are all fresh,
    // so the insert edge is the one taken, and the sibling `.Add` row plus the
    // duplicate-key shape below cover the other.
    expectStrKeyArmNoLeak(
        "if P.MakeDict().TryAdd(\"dd\", 4) then n := n + 1; end if;", 101);
}

BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           remove_on_an_unbound_owned_dictionary_is_reclaimed_spec_28_3) {
    expectStrKeyArmNoLeak(
        "if P.MakeDict().Remove(\"aa\") then n := n + 1; end if;", 101);
}

BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           clear_on_an_unbound_owned_dictionary_is_reclaimed_spec_28_3) {
    // `.Clear()` returns unit — its §28.3 obligation cannot sit at a return
    // site at all, which is exactly why this arm is pinned separately. It is
    // also the arm whose reclaim placement is most constrained: `bz_dict_clear`
    // empties the table but leaves the header, buckets and order spine
    // allocated (stdlib §4.8), so the free must follow the clear.
    expectStrKeyArmNoLeak("P.MakeDict().Clear();", -1);
}

// ===========================================================================
// HALF 2, IN ISOLATION: `.Count` has carried the reclaim call since #1013, so
// this row cannot be affected by Half 1 — it pins the helper's ADMISSION, and
// it is the row that fails if only the arms are fixed. On the parent it
// measured 42,925 B / 606 allocs — byte-identical to the arms above that never
// called the helper at all.
// ===========================================================================
BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           count_on_an_unbound_owned_string_keyed_dictionary_is_reclaimed_spec_28_3) {
    expectStrKeyArmNoLeak(
        "if P.MakeDict().Count > 0 then n := n + 1; end if;", 101);
}

// ===========================================================================
// HALF 1, GENERIC-KEYED: the key axis a string key cannot see. The helper
// admitted an int key before this fix (`.Count` is clean on the parent in this
// shape), so these rows fail ONLY if the arms' placement is what is wrong —
// which is what makes them the diagnostic mirror of the `.Count` row above.
// ===========================================================================
BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           generic_keyed_dictionary_arms_reclaim_the_unbound_receiver_spec_28_3) {
    expectGenKeyArmNoLeak(
        "if P.MakeDict().Contains(2) then n := n + 1; end if; "
        "P.MakeDict().Add(9, 9); "
        "if P.MakeDict().Remove(2) then n := n + 1; end if; "
        "P.MakeDict().Clear();", 202);
}

// ===========================================================================
// THE SIBLING HEAD. stdlib §4.17: `SortedDictionary<K,V>` shares the
// `bz_dict_t*` representation and dispatches through the identical
// `bz_dict_*` entry points — there is no SortedDictionary-specific member arm
// in the `isDict` block at all. This row is the DISPATCH-FACE control: it fails
// if the reclaim were keyed on a receiver name rather than on the table.
// ===========================================================================
BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           sorted_dictionary_receiver_reclaims_the_same_way_spec_4_17) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(
        "module M; "
        "class P public "
        "  static method MakeDict(): owned SortedDictionary<string, int> "
        "    var d: SortedDictionary<string, int> := "
        "        new SortedDictionary<string, int>(); "
        "    d.Add(\"aa\", 1); d.Add(\"bb\", 2); d.Add(\"cc\", 3); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      if P.MakeDict().Contains(\"bb\") then n := n + 1; end if; "
        "      P.MakeDict().Clear(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("101") != std::string::npos);
}

// ===========================================================================
// THE STRING-VALUE AXIS (§28.11). A `Dictionary<string, string>` co-owns a +1
// per stored VALUE, so a reclaim that freed the table without cascading would
// strand them (a leak) — and one that cascaded against a reference the table
// had never retained would free one twice (a crash). This row sees both
// directions: the leak oracle is armed, and ASan is on.
//
// This is the axis a `Dictionary<string, int>` row cannot see at all, since an
// `int` value carries no reference. On the parent the shape measured 6,464 B /
// 202 allocs at 101 iterations through the arms this fix reclaimed — the table
// half AND the value half. Every other release site in the backend already
// cascaded this value column (`.Remove`, `.Clear`, the indexer overwrite,
// `registerUsingDisposal`, `buildCollectionTempDisposal`); the unbound-
// temporary path was the one that did not.
// ===========================================================================
BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           string_valued_dictionary_arms_discharge_each_reference_once_spec_28_11) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, string> "
        "    var d: Dictionary<string, string> := "
        "        new Dictionary<string, string>(); "
        "    d.Add(\"aa\", \"va\" + \"a\"); d.Add(\"bb\", \"vb\" + \"b\"); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      if P.MakeDict().Contains(\"bb\") then n := n + 1; end if; "
        "      if P.MakeDict().Remove(\"aa\") then n := n + 1; end if; "
        "      var d := P.MakeDict(); "
        "      d.Add(\"cc\", \"vc\" + \"c\"); d.Clear(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("202") != std::string::npos);
}

// ===========================================================================
// THE BOUND CONTROL — the direction a reclaim ADDED where none existed can
// break. Each of these receivers is discharged by its own owner (an auto-drop
// at scope end, §28.3). If the reclaim's gate ever matched one of them, the
// table would be freed twice and ASan would report the double free or the
// use-after-free on the owner's own disposal. These rows are the regression
// test for that, not a restatement of the unbound rows — and they are also
// what proves the string-keyed admission did not over-widen: the gate is the
// unbound CallExpr shape, never the receiver's head name.
// ===========================================================================
BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           bound_dictionary_receivers_are_not_freed_by_the_member_arms_spec_28_3) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(
        strDictProducer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      var a := P.MakeDict(); "
        "      if a.Contains(\"bb\") then n := n + 1; end if; "
        "      a.Add(\"dd\", 4); "
        "      var b := P.MakeDict(); "
        "      if b.TryAdd(\"dd\", 4) then n := n + 1; end if; "
        "      var c := P.MakeDict(); "
        "      if c.Remove(\"aa\") then n := n + 1; end if; "
        "      var e := P.MakeDict(); "
        "      e.Clear(); "
        "      var f := P.MakeDict(); "
        "      if f.Count > 0 then n := n + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 1 (Contains) + 1 (TryAdd, a fresh key) + 1 (Remove) + 1 (Count) — the
    // `a`/`e` arms contribute no observable of their own.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("404") != std::string::npos);
}

// ===========================================================================
// THE OWNING-FAMILY CONTROL. `OwningDictionary<K,V>` is NOT in the borrowing
// head set (`isPlainBuiltinCollectionHead`), so it never reaches this helper —
// it is freed by `freeUnboundOwnedOwningCollectionTemp`, which CASCADES over
// the owned values (§28.11: "Disposing an OwningDictionary automatically
// disposes every value"). This row therefore asserts only what is in scope: the
// owning family's unbound receiver still COMPILES and still runs correctly
// through the same member arms. It is deliberately NOT a leak row — the owning
// family's own receiver reclaim is a separate mechanism, and asserting it clean
// here would pin a defect this fix does not claim.
//
// `runAot`, not `runAotChecked`: the point is the verdict and the observable,
// not memory. (A leak row's oracle must be armed; this one asserts none.)
// ===========================================================================
BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           owning_dictionary_member_arms_still_compile_and_run_spec_4_9) {
    JitResult r = runAot(
        "module M; "
        "public class Box "
        "  public V: string := \"\"; "
        "  public constructor(v: string) "
        "    V := v; "
        "  end constructor; "
        "end Box; "
        "class P public "
        "  static method MakeDict(): owned OwningDictionary<string, Box> "
        "    var d: OwningDictionary<string, Box> := "
        "        new OwningDictionary<string, Box>(); "
        "    d.Add(\"aa\", new Box(\"a\" + \"a\")); "
        "    d.Add(\"bb\", new Box(\"b\" + \"b\")); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      var d := P.MakeDict(); "
        "      if d.Contains(\"bb\") then n := n + 1; end if; "
        "      d.Clear(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("101") != std::string::npos);
}
// ===========================================================================
// THE ESCAPE AXIS (fix-forward #6419, §28.11 *Collection element ownership*,
// stdlib §4.8, v0.338).
//
// WHAT IS PINNED HERE, AND WHY THE FILE HEADER'S HALF-3 PARAGRAPH IS NOT THE
// WHOLE STORY. Half 3 cascades the §28.11 value column on every reclaim of a
// `Dictionary<K, string>` receiver, because the +1 was taken at `.Add` and
// `bz_dict_free`'s second argument walks exactly that column. What Half 3's
// proof counted was DISCHARGES against the table's own +1s. It said nothing
// about a reference the call site had already handed to the caller OUT of one
// of those same slots — and two sites do.
//
// `.First()`/`.Last()`/`.Single()`/`.ElementAt()` build the returned
// `DictionaryEntry<K,V>` with `decodeDictValueAt`, which for a pointer-typed V
// passes `e.val` through "as a borrow" (the runtime says the same in
// `bz_dict_values_str`'s own header: "the values themselves are owned by the
// dictionary and must NOT be freed by the caller"). The dict for-in loop's loop
// variable holds the same borrow, and a closure that captures it and ESCAPES
// the loop keeps it alive past the loop-exit reclaim.
//
// So on those arms dc3c8f2a freed the block the caller was about to read.
// MEASURED on dc3c8f2a, 101 iterations, `bezelc --sanitize=address`,
// ASAN_OPTIONS=detect_leaks=1:
//
//   var e := P.MakeDict().First(); n := n + e.Value.Length;   // Dictionary<string,string>
//   ==ERROR: AddressSanitizer: heap-use-after-free on address 0x7bbddcbe0048
//   READ of size 8 at 0x7bbddcbe0048 thread T0
//   freed by thread T0 here:
//       #0 ... in free
//       #1 0x0000003672a5 in bz$strElemRelease bezel
//   exit=1
//
// The base (dc3c8f2a^) runs the identical program to completion with correct
// output (`303`) and leaks only — so this is a regression dc3c8f2a INTRODUCED,
// not a gap it found. The three rows below pin all three reachable shapes, and
// `for_in_..._without_a_capture_still_discharges_each_reference_once` is the
// control that keeps the remedy from being the blunt one (declining the whole
// reclaim, which `freeFreshInputTempUnlessStringElementEscapes` does one block
// over for an `Array<string>` element terminal): that control would give back
// the leak fix for the common, non-capturing loop.
// ===========================================================================

// A string-keyed `Dictionary<string,string>` producer whose values are `+1`
// producers (concatenation), so each stored value carries the table's own +1.
static std::string strValProducer() {
    return
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, string> "
        "    var d: Dictionary<string, string> := "
        "        new Dictionary<string, string>(); "
        "    d.Add(\"aa\", \"va\" + \"a\"); d.Add(\"bb\", \"vb\" + \"b\"); "
        "    return d; "
        "  end MakeDict; "
        "end P; ";
}

BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           first_on_a_string_valued_dictionary_does_not_free_the_entry_it_returns_spec_28_11) {
    // 3 (the first entry's value "vaa") x 101.
    expectNoMemoryError(
        strValProducer() +
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
        "end C; ",
        "303");
}

// The int-keyed twin. It is a separate row and not a variation for its own
// sake: `e.val` is the value column for BOTH key modes, but the two key modes
// reach this arm through DIFFERENT snapshot calls (`bz_dict_values_str` vs
// `bz_dict_values_gen`), and a fix that gated only on the key spelling would
// leave one of them crashing.
BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           first_on_an_int_keyed_string_valued_dictionary_does_not_free_the_entry_it_returns_spec_28_11) {
    expectNoMemoryError(
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<int, string> "
        "    var d: Dictionary<int, string> := new Dictionary<int, string>(); "
        "    d.Add(1, \"va\" + \"a\"); d.Add(2, \"vb\" + \"b\"); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
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
        "end C; ",
        "303");
}

// The dict for-in loop's own reclaim (LlvmBackend_Stmts.cpp), with an ESCAPING
// capture. This is the shape the loop's own Key release is already gated on
// (`forVarCapturedEscaping`, fix #4161) — the Value needed the same authority,
// which is why the fix-forward passes that predicate rather than inventing a
// second one. Ordinary completion, no `return` anywhere: the closure is
// assigned to an outer local and called after the loop has already reclaimed
// the table.
BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           for_in_over_a_string_valued_dictionary_with_an_escaping_capture_does_not_free_the_entry_spec_28_11) {
    expectNoMemoryError(
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, string> "
        "    var d: Dictionary<string, string> := "
        "        new Dictionary<string, string>(); "
        "    d.Add(\"aa\", \"va\" + \"a\"); d.Add(\"bb\", \"vb\" + \"b\"); "
        "    return d; "
        "  end MakeDict; "
        "  static method MakeClosure(): () -> int "
        "    var f: () -> int := lambda() => 0; "
        "    for var entry in P.MakeDict() do "
        "      f := lambda() => entry.Value.Length; "
        "    end for; "
        "    return f; "
        "  end MakeClosure; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      var g := P.MakeClosure(); "
        "      n := n + g(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ",
        "303");
}

// THE CONTROL, and the row that decides between the two candidate remedies.
// The NON-capturing twin of the loop above must stay LEAK-FREE: with the
// disposer installed (dc3c8f2a's behaviour, and unchanged by this fix-forward)
// it measures exit 0, correct output and NO leak. A remedy that declined the
// value discharge on this arm unconditionally — the blunt reading of "a
// for-in loop's entries are borrows" — would leave 101 x 2 strings outstanding
// and fail here. So the gate is the escape, not the arm.
//
// 6 (both entries, 3 each) x 101 = 606.
//
// Proven non-vacuous the same way: built with the blunt remedy (the escape gate
// made unconditional at this site) the same loop measures 7,272 B / 404 allocs
// under LSan and this row reports `assertion failed: r.ran`. So the gate is
// measured to be the escape and not the arm.
BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           for_in_over_a_string_valued_dictionary_without_a_capture_still_discharges_each_reference_once_spec_28_11) {
    expectNoLeak(
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
        "end C; ",
        "606");
}

// THE TABLE IS STILL RECLAIMED, AND THE VALUE COLUMN IS NOW DISCHARGED. This
// row began life as a LEAK CEILING — the only observable that could
// distinguish "declined the value DISCHARGE" from "declined the whole reclaim"
// back when #6419's escape axis had to decline the discharge (its measured
// 7,272 B / 404 allocs sat ~2.7x below a 20,000 B ceiling and ~2.5x above the
// 49,894 B / 909 allocs of the blunt remedy it rejected). A ceiling passes at
// 0 exactly as readily as at 7,272, so as an ORACLE it was a tolerance, not an
// assertion: it could not tell "discharged" from "declined".
//
// fix #6470 (v0.338) removed the reason for the tolerance. The terminal now
// MINTS the +1 on the decoded value (so the returned entry's Value is the
// caller's own reference rather than a borrow of the receiver's `e.val`,
// per §28.11's operations table reading a shared-reference slot as "reference
// out"), the reclaim discharges the value column in full, and a
// `dictEntryValueRelease` Disposal discharges the caller's +1 at the entry
// local's scope end. Nothing leaks on this shape any more, so the bound is
// tightened from a ceiling to an EXACT ZERO.
//
// Why zero is a sound bound rather than a precision claim: LSan is conservative
// and its totals jitter, which is exactly why the sibling rows pin ceilings.
// But jitter only moves the number when bytes are genuinely LIVE-but-unreachable
// under LSan's scan, and every byte this fix could strand was unreachable the
// moment `bz_dict_free` returned — so the failing direction (LSan reporting
// MORE than the truth) is the only one reachable here, and it reports strictly
// more, never less. A tight `== 0` is therefore the correct bound at this
// regime, and it is the only assertion that would have failed on #6419's
// parent: MEASURED 7272 B / 404 allocs there, 0 B after.
//
// The exemption row this used to justify itself against
// (`scripts/gate/sanitizer_leak_exempt.txt`, `status:defect issue:6470`) is
// DELETED in the same commit — the defect it recorded is fixed, and the file's
// last `defect` row leaves it at 0 defect / 32 trade / 9 residue. The table
// free this row also pins is untouched by the fix: the header, bucket array,
// insertion-order spine and per-key byte copies are still reclaimed, and the
// returned entry points into none of them (its Key is rebuilt fresh by
// `decodeDictStringKeyAt`; its Value is the separate reference the terminal
// minted).
//
// (The `dc3c8f2a` caveat recorded on the old ceiling still stands and is why
// the UAF this row could not see is pinned by the three crash-oracle rows
// above, on a separate oracle: a use-after-free suppresses the leak summary
// entirely, so `leakedBytes` would report 0 for it.)
BEZEL_TEST(dict_terminal_unbound_reclaim_6419,
           first_still_reclaims_the_dictionary_backing_on_an_escaping_value_arm_spec_28_3) {
    const long leaked = leakedBytes(
        strValProducer() +
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
        "end C; ");
    BEZEL_EXPECT_EQ(leaked, 0);
}
