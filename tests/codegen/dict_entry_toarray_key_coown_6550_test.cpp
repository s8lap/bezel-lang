// Codegen — a `Dictionary<string, V>.ToArray()` ENTRY SNAPSHOT handed Keys the
// LINQ pipeline had already released, so `arr[i].Key` read after the drain was
// a heap-use-after-free (#6550; §28.11 *Collection element ownership*, §28.9,
// §9.7, stdlib §4.8/§5, v0.338).
//
// THE OWNERSHIP CONTRACT, WHICH IS THE WHOLE QUESTION.
//
// stdlib §4.8 declares NO `ToArray` on `Dictionary<K, V>`, so `d.ToArray()`
// resolves to stdlib §5's `ToArray(): owned Array<T>` with
// `T = DictionaryEntry<K, V>` — a FRESH snapshot array, owned by the caller.
// §28.11's element-category table puts a record in the VALUE row, and its
// OPERATIONS table's value column reads copy-store "copies the value in",
// `Dispose()` "no user-visible work", indexer-get "copy out". So the snapshot's
// element DISPOSAL is empty: nothing in the element policy will ever discharge
// its entries' Keys. The spec answers this question; no `spec-question` issue is
// warranted.
//
// §28.9 is where the machinery lives: a `string` is runtime-managed, and "how the
// runtime manages their lifecycle (reference counting, region allocation, or
// other) is not part of this specification". This runtime chose refcounting, and
// that is the half that has to be symmetric. §9.7 says why a CO-OWNED reference
// is the right shape rather than a borrow: a reference-typed record field whose
// DECLARED type is runtime-managed does not make the record borrow-bearing
// "because runtime-managed values have unbounded lifetime by construction". So a
// snapshot that OUTLIVES the pipeline which produced it must hold the reference
// itself.
//
// MEASURED that the pipeline's half was already there and correct on its own:
// `lowerToLazyEnumerator`'s Dictionary arm rebuilds each Key as a fresh
// `bz_string` (`initStringRc` → rc = 1) and wraps the entry array with
// `bz_lazy_from_owned_str_dict_array` (#2544), so `bz_lazy_dispose` releases
// every rebuilt Key. That is right — those keys never escape a for-in loop, a
// `First()`, a `Count()`, or any other consumer whose entries die with the
// enumerator.
//
// THE DEFECT, IN TWO HALVES THAT ARE ONLY CORRECT TOGETHER. `bz_lazy_to_array`
// drains by copying each element's BYTES into the fresh result array — for a
// `DictionaryEntry<string, V>` that copy carries the Key POINTER at struct field
// 0 — and THEN calls `bz_lazy_dispose`. So the result array was handed to the
// caller holding pointers to freed strings. MEASURED on this lane's base
// (4aaa4f21): `AddressSanitizer: heap-use-after-free`, READ of size 8, freed by
// `bz_lazy_release_str_dict_keys` <- `bz_lazy_dispose`, on every one of
// `d.ToArray()`, `d.Where(...).ToArray()`, `d.Take(n).ToArray()`,
// `d.Select(lambda(e) => e).ToArray()` and `SortedDictionary<string, V>
// .ToArray()`.
//
// WHY A CO-OWNERSHIP INSIDE THE DRAIN RATHER THAN A RETAIN AT THE CALL SITE.
// The obvious fix — emit a `bz_array_retain_strings`-shaped retain after the
// drain call — was tried and MEASURED RED: `bz_lazy_to_array` drains AND disposes
// internally, so by the time codegen holds the result array every Key is already
// freed and a retain would bump the refcount word of freed memory. Ordering is
// the whole fix, so the reference has to be taken between the drain loop and the
// dispose. Hence `bz_lazy_to_array_str_dict_keys`, a drain that takes the
// snapshot's +1 while the pipeline still holds its own: each Key goes 1 → 2 → 1
// across the drain and 1 → 0 when the snapshot array is freed.
//
// WHY THE DISCHARGE LIVES IN THE ARRAY HEADER. `bz_array_free`'s per-element
// disposer is a `void(*)(void*)` applied to the slot at STRIDE 8
// (`((void**)arr->data)[i]`), which for the 16-byte `{bz_string*, V}` entry
// reaches neither field — a limitation `wrapDictionaryEntryArrayAsIEnumerable`'s
// Dispose already documents before doing the same walk by hand. And
// `bz_array_t` records no `elem_size`. So `bz_array_own_str_dict_keys` stores the
// stride in a new appended header field and `bz_array_free` /
// `bz_array_mark_disposed` walk with `bz_array_ptr_at`, exactly as the working
// enumerable-side Dispose does.
//
// WHY THE GATE IS PROVENANCE, NOT ELEMENT TYPE. The stamp is set only by a
// producer that actually took a reference, never inferred from the element type.
// An `Array<DictionaryEntry<string, V>>` a user builds with `Add` or
// `new DictionaryEntry(...)` has no such obligation under §28.11's value row and
// must stay unstamped, or its Keys would be released without ever having been
// retained. The stamp is calloc-zeroed, so an unstamped array is byte-for-byte
// unaffected by the discharge.
//
// THE CONTROLS BELOW ARE THE GATE'S NEGATIVE SPACE and are the rows that would
// fail if this were keyed on the element type alone: an int-keyed snapshot (Keys
// are inline bytes with no heap ownership, so the source never releases them and
// there is nothing to co-own), `Select(lambda(e) => e.Key)` (the produced element
// is a bare `string`, already handled by the `bz_array_retain_strings` intercept —
// stamping it too would retain every element twice and release it once), a
// `.Count`-only materialize, and the for-in / `First()` consumers whose entries
// never escape the pipeline.
//
// THE ORACLE TRAP THIS FILE GUARDS. A heap-use-after-free ABORTS the process
// before LeakSanitizer prints its summary, so a leak assertion alone also reads
// as "clean" for a crash. Every leak row is therefore paired with a crash-oracle
// row (`expectRunsClean`) that asserts the program RAN TO COMPLETION — `r.ran` is
// false if and only if the generated program died on a memory error — and asserts
// the exact stdout, because a fix that returns the right bytes by accident is
// still a fix that must be pinned. And `runAotCheckedLenient` passes
// `detect_leaks=0` (aot_support.hpp), so a leak assertion written against it is
// VACUOUS; every leak assertion here goes through `runAotChecked`, which
// hardcodes `detectLeaks = true`.
//
// A CORRECTION, because the previous version of this header claimed
// "`BEZEL_AOT_DETECT_LEAKS` defaults OFF, so every leak row arms it explicitly
// through `LeakEnv`; `runAotChecked` without that arm would be a VACUOUS leak
// oracle." MEASURED FALSE: `runAotCheckedImpl` never reads
// `BEZEL_AOT_DETECT_LEAKS` at all — `runAotChecked` passes `/*detectLeaks=*/true`
// as a hardcoded argument (aot_support.hpp), so the harness arms LeakSanitizer
// unconditionally and the `LeakEnv` scopes in this file were INERT. The
// conclusion the claim reached (use `runAotChecked`, not the Lenient variant) is
// right and is why every row below still passes; the mechanism was wrong, and a
// false model of the oracle is worse than no comment. The same false claim
// appears in `dict_entry_value_refcount_6470_test.cpp` and
// `dict_terminal_unbound_reclaim_6419_test.cpp`, which are NOT this lane's files;
// they are filed rather than edited.
#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

#include <cstdlib>
#include <regex>
#include <string>

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// Save/restore BEZEL_AOT_DETECT_LEAKS around a row.
//
// IT IS INERT, and deliberately kept only so the rows read the same as they did
// before the header correction above. `runAotCheckedImpl` never consults this
// variable — it takes `detectLeaks` as a hardcoded argument — so this scope
// changes nothing about the leak oracle. Delete it once the rows that carry it
// are next edited; do NOT read it as "this row's leak checking is armed here",
// because it is `runAotChecked` vs `runAotCheckedLenient` that decides that.
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

// The harness SKIPS (loudly, counted) when clang is absent; a row that asserts
// through that would be asserting on an unarmed oracle. A sanitizer finding is
// NOT a skip — "sanitizer fired" means it ran and found something.
bool skipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// 101 iterations — a one-shot leak repro is a vacuity trap, and the leak this
// fix prevents is per-materialization: one escape is a single freed Key, which
// is small enough to hide under LSan jitter.
const char* kIterations = "i < 101";

// The producer returns a FRESH unbound `owned` dictionary each call, so the
// entry snapshot in every iteration is a distinct array with distinct rebuilt
// Keys — a single reused table would let one materialization's error stand in
// for all 101.
//
// THE KEYS ARE LITERALS AND THAT IS CORRECT, not a weakness: the Keys this file
// is about are not the dictionary's stored key bytes (stdlib §4.8 stores those
// inline) but the `bz_string`s `lowerToLazyEnumerator`'s Dictionary arm REBUILDS
// from those bytes when it materializes the entry array. Every rebuilt key gets
// `initStringRc` → rc = 1, so it is a genuine heap string that the pipeline
// owns and must release — MEASURED, the base reproduces the use-after-free with
// exactly these literals.
std::string producer() {
    return
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, int> "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"aa\", 1); d.Add(\"bb\", 2); "
        "    return d; "
        "  end MakeDict; "
        "end P; ";
}

// A CRASH-oracle row: the program must RUN TO COMPLETION, print `expected`, and
// exit 0. This is the oracle that catches the heap-use-after-free, which the
// leak oracle structurally cannot (a UAF aborts before LSan prints). `r.ran` is
// the load-bearing assertion: it is false iff the generated program died on a
// memory error.
void expectRunsClean(const std::string& program, const char* expected) {
    JitResult r = runAotCheckedLenient(program);
    // Loud-first, silent-second, in that order — never `if (skipped) return;`
    // before the assertion.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(expected) != std::string::npos);
}

// A LEAK-oracle row: strict LSan armed, and the program must run clean. This is
// the row that pins the OTHER half — a fix that over-retains, or that discharges
// the snapshot's Keys twice, passes every crash row above and fails here.
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
// THE ISSUE'S SHAPE: materialize, then read a Key AFTER the drain.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           toarray_entry_snapshot_key_read_is_not_use_after_free_spec_28_11) {
    expectRunsClean(producer() +
        "class C public "
        "  static method Main(): int "
        "    var arr := P.MakeDict().ToArray(); "
        "    Console.WriteLine($\"{arr[0].Key} {arr.Count}\"); "
        "    return 0; "
        "  end Main; "
        "end C; ",
        "aa 2");
}

// The symmetry half. Zero, not a ceiling: a ceiling passes at 0 as readily as
// at a large number and so cannot distinguish "co-owned and discharged" from
// "declined to own". This fix co-owns AND discharges, and zero is the one bound
// LSan jitter cannot move in the failing direction here — every byte the base
// strands here is unreachable the moment the array's free returns.
//
// BOTH KEYS ARE READ, not just the first: a drain that retained only slot 0
// would satisfy a one-slot row.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           toarray_entry_snapshot_co_owns_keys_leaks_nothing_spec_28_9) {
    expectNoLeak(producer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      var arr := P.MakeDict().ToArray(); "
        "      n := n + arr[0].Key.Length + arr[1].Key.Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ",
        "404");
}

// THE SAME DEFECT THROUGH THE OTHER OPERATORS THAT LET THE ENTRY ESCAPE. Each of
// these was MEASURED use-after-free on the base (4aaa4f21) at the same free
// site; the escape does not depend on which operator assembled the pipeline, only
// on the snapshot outliving it. `Where` and `Take` are element-preserving;
// `Select(lambda(e) => e)` is the identity projection that still copies the entry
// struct (so the Key pointer rides along); `SortedDictionary` is stdlib §4.12's
// ordered sibling, materialized by the same arm.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           where_toarray_entry_snapshot_key_read_spec_28_11) {
    expectRunsClean(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string, int>(); "
        "  d.Add(\"aa\", 1); d.Add(\"bb\", 2); "
        "  var arr := d.Where(lambda(kv) => kv.Value > 1).ToArray(); "
        "  Console.WriteLine($\"{arr[0].Key} {arr.Count}\"); "
        "  return 0; "
        "end Main; end C; ",
        "bb 1");
}

BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           take_toarray_entry_snapshot_key_read_spec_28_11) {
    expectRunsClean(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string, int>(); "
        "  d.Add(\"aa\", 1); d.Add(\"bb\", 2); "
        "  var arr := d.Take(1).ToArray(); "
        "  Console.WriteLine($\"{arr[0].Key} {arr.Count}\"); "
        "  return 0; "
        "end Main; end C; ",
        "aa 1");
}

BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           select_identity_toarray_entry_snapshot_key_read_spec_28_11) {
    expectRunsClean(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string, int>(); "
        "  d.Add(\"aa\", 1); d.Add(\"bb\", 2); "
        "  var arr := d.Select(lambda(e) => e).ToArray(); "
        "  Console.WriteLine($\"{arr[0].Key} {arr.Count}\"); "
        "  return 0; "
        "end Main; end C; ",
        "aa 2");
}

BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           sorted_dictionary_toarray_entry_snapshot_key_read_spec_4_12) {
    expectRunsClean(
        "module M; class C public static method Main(): int "
        "  var d := new SortedDictionary<string, string>(); "
        "  d.Add(\"b\", \"1\"); d.Add(\"a\", \"2\"); "
        "  var arr := d.ToArray(); "
        "  Console.WriteLine($\"{arr[0].Key} {arr.Count}\"); "
        "  return 0; "
        "end Main; end C; ",
        "a 2");
}

// ---------------------------------------------------------------------------
// fix #6560 — THE MUTATION ROWS. #6550 discharged `count` slots at free, which
// is correct for an array nobody touched and wrong the moment anyone does. Both
// directions of drift were reachable from ordinary Bezel, and neither is a leak:
// GROWTH released a reference nobody owned (a §28.9 over-release ABORT, rc 134,
// on a five-line program that ran to completion before #6550), and SHRINKAGE
// stranded every retained Key.
//
// The invariant these rows pin is stated over the PRESENT, not the past:
//
//   A STAMPED ARRAY CO-OWNS EXACTLY ONE REFERENCE ON THE Key OF EVERY SLOT IN
//   [0, count).
//
// so each mutator maintains it where it changes the population — an insert takes,
// a remove or clear gives back. That is why the fix is the mutator half and not
// a free-time re-derivation: a retained-count or ownership BITMAP would have to
// be shifted by `RemoveAt`, appended to by `Add`, and would have to survive
// `Clear` — the one operation that erases the record of what was owned.

// The ORACLE FOR THE ABORT, which is the assertion that matters most here. A
// refcount over-release is not a leak: `bz_refcount_overrelease_fail` (#4547)
// prints `bezel: fatal: refcount over-release (string) ... terminating per
// §28.9` and aborts the process. So every mutation row asserts all three of
// `ran`, `exitCode == 0`, and the ABSENCE of that banner — asserting only the
// leak bound would pass here, because a hard abort reports no leak at all.
//
// 200 iterations: one reproduction is a single extra release, which is small
// enough to hide under refcount noise, and the defect repeats per operation.
const char* kMutations = "i < 200";

// Every entry is rebuilt by the Dictionary materialization, so every mutation
// below moves a genuine heap `bz_string`, not a literal sentinel (which would
// make retain/release no-ops and every row vacuous).
std::string mutationProducer() {
    return
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, int> "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"aa\", 1); d.Add(\"bb\", 2); d.Add(\"cc\", 3); "
        "    return d; "
        "  end MakeDict; "
        // A SECOND dictionary, so the entry handed to `Add` comes from another
        // snapshot that co-owns its own reference — the transfer case that made
        // the pre-#6560 free-time walk release a reference nobody owned.
        "  static method MakeOther(): owned Dictionary<string, int> "
        "    var d := new Dictionary<string, int>(); "
        "    d.Add(\"zz\", 9); d.Add(\"yy\", 8); "
        "    return d; "
        "  end MakeOther; "
        "end P; ";
}

// THE ABORT ROW. `a1.Add(a2[0])` appends a slot whose Key `a2` co-owns; before
// #6560 `a1`'s free-time walk released it as if `a1` owned it too.
//
// ORACLE CHOICE, and it is load-bearing: `runAotCheckedLenient` passes
// `detect_leaks=0` (aot_support.hpp — "LeakSanitizer DISABLED at runtime … The
// *Lenient variant"), so a leak assertion written against it is VACUOUS. This
// helper asserts a LEAK BOUND, so it must use `runAotChecked`, which runs
// LeakSanitizer with `exitcode=99:abort_on_error=0`: a leak exits 99 and prints
// the report, and an over-release — a real `abort()` from
// `bz_refcount_overrelease_fail` (#4547), not a sanitizer diagnostic — kills the
// process so `r.ran` goes false and the stdout never appears. All three of
// `ran`, `exitCode == 0` and the banner check are asserted because the two
// failure modes report through different channels.
//
// MEASURED trap this row exists to catch: with `detect_leaks=0` the Clear and
// RemoveAt rows below both PASSED against the broken runtime — a leak reported
// by nothing. The standalone `bezelc --sanitize=address` binary for the Clear
// row leaked `21000 byte(s) in 1200 allocation(s)` at the same instant the
// harness called it clean.
void expectNoOverRelease(const std::string& program, const char* expected) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(expected) != std::string::npos);
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("over-release") == std::string::npos);
        BEZEL_EXPECT_TRUE(d.message.find("byte(s) leaked") == std::string::npos);
    }
}

BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           add_of_a_borrowed_entry_does_not_over_release_spec_28_9) {
    expectNoOverRelease(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string, int>(); "
        "  d.Add(\"aa\", 1); d.Add(\"bb\", 2); "
        "  var a1 := d.ToArray(); var a2 := d.ToArray(); "
        "  a1.Add(a2[0]); "
        "  Console.WriteLine($\"{a1.Count} {a1[0].Key}\"); "
        "  return 0; "
        "end Main; end C; ",
        "3 aa");
}

// `Clear` erases `count`, which is the only record of which slots were owned,
// so the discharge has to happen IN `Clear`. Pre-#6560 MEASURED 21000 bytes in
// 1200 allocations over these 200 iterations; the expected value is 0.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           clear_releases_the_keys_it_drops_spec_28_11) {
    expectNoOverRelease(
        mutationProducer() +
        "class C public static method Main(): int "
        "  var i := 0; var n := 0; "
        "  while " + std::string(kMutations) + " do "
        "    var arr := P.MakeDict().ToArray(); "
        "    arr.Clear(); "
        "    n := n + arr.Count; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C; ",
        "0");
}

// `RemoveAt` must give back the dropped slot AND leave the survivors owned —
// ownership follows the SLOT, so the shift needs no further bookkeeping. Both
// survivors' Keys are READ here, so a fix that released the whole array on a
// remove (instead of one slot) fails on the read, not only on the bound.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           remove_at_releases_only_the_dropped_key_spec_28_11) {
    expectNoOverRelease(
        mutationProducer() +
        "class C public static method Main(): int "
        "  var i := 0; var n := 0; "
        "  while " + std::string(kMutations) + " do "
        "    var arr := P.MakeDict().ToArray(); "
        "    arr.RemoveAt(1); "
        "    n := n + arr[0].Key.Length + arr[1].Key.Length; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C; ",
        "800");  // 200 x ("aa".Length + "cc".Length) = 200 x 4
}

// An indexer store REPLACES the slot: give back the Key the array co-owned,
// take the one it now co-owns. Before #6560 neither happened, so the displaced
// Key leaked and the incoming one was released without ever being retained.
// The expected value proves the store actually landed ("zz", not the displaced
// "aa") — a wrong-answer fix would pass a leak bound alone.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           indexer_set_swaps_key_ownership_spec_28_11) {
    expectNoOverRelease(
        mutationProducer() +
        "class C public static method Main(): int "
        "  var arr := P.MakeDict().ToArray(); "
        "  var other := P.MakeOther().ToArray(); "
        "  arr[0] := other[0]; "
        "  Console.WriteLine($\"{arr.Count} {arr[0].Key} {arr[1].Key}\"); "
        "  return 0; "
        "end Main; end C; ",
        "3 zz bb");
}

// All three mutators, interleaved, on the SAME array. This is the row that a
// per-mutator patch tends to miss: the `RemoveAt` that precedes the `Add` must
// have left the survivors owned, and the `Add` must take on what it appends.
// (This comment previously also claimed the `Add` "re-establishes" the stamp
// `Clear` cleared — MEASURED FALSE, see `bz_array_clear` in the runtime: the
// stamp does not come back, so a post-`Clear` `Add` takes nothing and the array
// is on the ordinary unstamped footing. The row below still pins the sequence's
// balance, because an unstamped array neither takes nor gives back and so
// cannot over-release.) Expected 200 x ("yy".Length).
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           mixed_add_remove_clear_sequence_spec_28_11) {
    expectNoOverRelease(
        mutationProducer() +
        "class C public static method Main(): int "
        "  var i := 0; var n := 0; "
        "  while " + std::string(kMutations) + " do "
        "    var arr := P.MakeDict().ToArray(); "
        "    var other := P.MakeOther().ToArray(); "
        "    arr.Add(other[0]); "
        "    arr.RemoveAt(0); "
        "    arr.Clear(); "
        "    arr.Add(other[1]); "
        "    n := n + arr[0].Key.Length; "
        "    i := i + 1; "
        "  end while; "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C; ",
        "400");
}

// ---------------------------------------------------------------------------
// THE GATE'S NEGATIVE SPACE. A fix keyed on the produced element STRIDE, or on
// "the element is a 2-field struct whose field 0 is a pointer", passes every row
// above and fails these.

// An INT-keyed snapshot: stdlib §4.8 stores int keys inline, so there is no heap
// ownership for the pipeline to hold and none for the snapshot to take. A gate
// that fired here would retain and then release a pointer that is not a string.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           control_int_keyed_snapshot_is_untouched_spec_4_8) {
    expectNoLeak(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<int, string>(); "
        "  d.Add(7, \"x\"); d.Add(8, \"y\"); "
        "  var arr := d.ToArray(); "
        "  Console.WriteLine($\"{arr[0].Key} {arr.Count}\"); "
        "  return 0; "
        "end Main; end C; ",
        "7 2");
}

// `Select(lambda(e) => e.Key)` produces a bare `string`, NOT an entry, and is
// already covered by the `bz_array_retain_strings` intercept. Stamping it as an
// entry snapshot as well would retain every element TWICE and release it once —
// a leak this row catches. It is a CRASH row only: it carries a SEPARATE,
// pre-existing 68-byte leak (the rebuilt Keys are released by neither the
// pipeline — its `Select` node never disposes its source — nor the produced
// `Array<string>`), which is filed as its own issue and is deliberately NOT
// asserted here. Asserting it would pin a defect this fix does not claim.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           control_select_key_toarray_is_not_double_retained_spec_28_11) {
    expectRunsClean(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string, int>(); "
        "  d.Add(\"aa\", 1); d.Add(\"bb\", 2); "
        "  var arr := d.Select(lambda(e) => e.Key).ToArray(); "
        "  Console.WriteLine($\"{arr[0]} {arr.Count}\"); "
        "  return 0; "
        "end Main; end C; ",
        "aa 2");
}

// A `.Count`-only materialize reads no Key, so it is the row that separates "the
// snapshot co-owns and discharges" from "the snapshot silently retains forever":
// this fix takes a reference and gives it back, so the count is still leak-free.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           control_count_only_materialize_leaks_nothing_spec_28_11) {
    expectNoLeak(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string, string>(); "
        "  d.Add(\"a\", \"1\"); d.Add(\"b\", \"2\"); "
        "  Console.WriteLine($\"{d.ToArray().Count}\"); "
        "  return 0; "
        "end Main; end C; ",
        "2");
}

// The for-in and `First()` consumers: their entries die WITH the enumerator, so
// the pipeline's release is their only correct discharge. A fix that moved the
// reference out of the pipeline — rather than adding a second one — would strand
// or double-release here.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           control_forin_entry_key_read_unchanged_spec_4_8) {
    expectNoLeak(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string, string>(); "
        "  d.Add(\"a\", \"1\"); d.Add(\"b\", \"2\"); "
        // Accumulate into one line rather than printing per iteration:
        // `Console.WriteLine` terminates the line, so a per-iteration print
        // yields "a \nb \n\n" and the expected substring has to carry the
        // newlines. One accumulator makes the assertion a single token.
        "  var s := \"\"; "
        "  for var e in d do s := s + e.Key + \",\"; end for; "
        "  Console.WriteLine(s); "
        "  return 0; "
        "end Main; end C; ",
        "a,b,");
}

BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           control_first_entry_key_read_unchanged_spec_4_8) {
    expectNoLeak(
        "module M; class C public static method Main(): int "
        "  var d := new Dictionary<string, string>(); "
        "  d.Add(\"a\", \"1\"); d.Add(\"b\", \"2\"); "
        "  var e := d.First(); "
        "  Console.WriteLine($\"{e.Key} {e.Value}\"); "
        "  return 0; "
        "end Main; end C; ",
        "a 1");
}

// ---------------------------------------------------------------------------
// THE ALIASING SHAPES — the blind spot in the row above.
//
// `indexer_set_swaps_key_ownership_spec_28_11` stores a key that came from a
// DIFFERENT snapshot, so its incoming Key and the displaced Key are always two
// different objects. That makes it blind to the whole class where the two are
// the SAME object, which §28.11's value row makes the COMMON case rather than an
// exotic one: the indexer-get is a plain "copy out", so a value copied out of a
// stamped array — into a local, or into another slot — carries the very
// `bz_string*` the source slot holds. `arr[0] := arr[0]` is then not "replace A
// with A"; it is the identity, and the store's take and drop must cancel.
//
// #6560 first shipped the store as drop → copy → take, which is wrong precisely
// here: the drop drives the shared Key's count to 0 and FREES it, the copy
// writes the now-dangling pointer back, and the take retains freed memory.
// MEASURED on the rejected tip `0ff67af8` (both legs instrumented —
// `nm -D | grep -c __asan` = 182 vs 0, `ASAN_OPTIONS=detect_leaks=0:exitcode=99:
// abort_on_error=0`): `AddressSanitizer: heap-use-after-free`, READ of size 8,
// freed by `bz_string_release` <- `bz_array_drop_str_dict_key`
// (bezel_runtime_collections.cpp:164) <- `bz_array_set` (:279), rc=99 on all
// three rows below; the distinct-Key control measured clean on the same leg.
//
// THE FIX is the ORDER, not a special case: take the incoming Key first, then
// copy, then release the DISPLACED one, so the count never passes through zero
// while the Key is still reachable from either side. Self-assignment then nets
// to a no-op (1 → 2 → 1).

// The identity store. Nothing about the array's contents may change, and the Key
// must still be readable afterwards.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           indexer_set_self_assign_keeps_the_key_alive_spec_28_11) {
    expectNoOverRelease(
        mutationProducer() +
        "class C public static method Main(): int "
        "  var arr := P.MakeDict().ToArray(); "
        "  arr[0] := arr[0]; "
        "  Console.WriteLine($\"{arr.Count} {arr[0].Key} {arr[1].Key}\"); "
        "  return 0; "
        "end Main; end C; ",
        "3 aa bb");
}

// THE SAME ALIAS, one indirection out: §28.11's "copy out" puts the Key pointer
// into a local, and storing the local back stores the same pointer. This is the
// shape a swap's first half produces, and it is the one that distinguishes a
// fix which compares the two pointers from one which merely reorders them —
// under a reorder-only fix the local round-trip still passes (take-then-release
// nets to zero on the same object), so BOTH this row and the self-assign row are
// needed to pin the behaviour rather than the implementation.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           indexer_set_local_roundtrip_keeps_the_key_alive_spec_28_11) {
    expectNoOverRelease(
        mutationProducer() +
        "class C public static method Main(): int "
        "  var arr := P.MakeDict().ToArray(); "
        "  var e := arr[0]; "
        "  arr[0] := e; "
        "  Console.WriteLine($\"{arr.Count} {arr[0].Key}\"); "
        "  return 0; "
        "end Main; end C; ",
        "3 aa");
}

// TWO SLOTS, ONE KEY. `arr[0] := arr[1]` is the store's other direction: the
// array ends up holding the same `bz_string*` twice, so that Key's count is 2
// and the free must give back exactly two references. Under the drop → copy →
// take shape the FIRST half of this store released a reference the second slot
// still counted, and the free released the survivor once too many — the §28.9
// over-release abort, MEASURED on the parent leg `d8d81745` as
// `bezel: fatal: refcount over-release (string) — two releases for one
// reference; terminating per §28.9`, rc=134. Take-then-release makes the two
// halves cancel at the refcount rather than at the allocator.
//
// Both directions are here because they are the two halves of a swap, and a
// fix that only balances one of them still over-releases on the other.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           indexer_set_duplicate_slot_does_not_over_release_spec_28_9) {
    expectNoOverRelease(
        mutationProducer() +
        "class C public static method Main(): int "
        "  var arr := P.MakeDict().ToArray(); "
        "  arr[0] := arr[1]; "
        "  Console.WriteLine($\"{arr.Count} {arr[0].Key} {arr[1].Key}\"); "
        "  return 0; "
        "end Main; end C; ",
        "3 bb bb");
}

BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           indexer_set_duplicate_slot_reverse_does_not_over_release_spec_28_9) {
    expectNoOverRelease(
        mutationProducer() +
        "class C public static method Main(): int "
        "  var arr := P.MakeDict().ToArray(); "
        "  arr[1] := arr[0]; "
        "  Console.WriteLine($\"{arr.Count} {arr[0].Key} {arr[1].Key}\"); "
        "  return 0; "
        "end Main; end C; ",
        "3 aa aa");
}

// ---------------------------------------------------------------------------
// THE MUTATOR SWEEP — every other slot-moving entry point, measured rather than
// assumed. Each of these MOVES or REPLACES a Key, so each was a candidate for
// the same drop-then-re-take aliasing shape. They are clean, and the reason is
// worth recording rather than leaving to a future reader's suspicion:
//
//   * `Add` and `Insert` only ever ADD a slot; there is no displaced Key to give
//     back, so there is no drop/take pair to order. They take AFTER the copy,
//     which is the only safe order for them regardless of aliasing.
//   * `RemoveAt` and `drain_next_lifo` only ever DISCARD a slot, so the dropped
//     Key is one the array is genuinely losing; a survivor that happens to share
//     it holds its own separate reference, and the memmove shifts slot addresses
//     without touching any count.
//   * `Clear` discharges every slot, which is a whole-population walk.
//
// These rows are what turns "I checked the others" into a pinned fact.

// `arr.Add(arr[0])` — appending a slot whose Key another slot of the SAME array
// already co-owns. The appended slot must take its own reference, so the shared
// Key's count is 2 and the free balances.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           add_of_a_slot_of_the_same_array_balances_spec_28_9) {
    expectNoOverRelease(
        mutationProducer() +
        "class C public static method Main(): int "
        "  var arr := P.MakeDict().ToArray(); "
        "  arr.Add(arr[0]); "
        "  Console.WriteLine($\"{arr.Count} {arr[0].Key} {arr[3].Key}\"); "
        "  return 0; "
        "end Main; end C; ",
        "4 aa aa");
}

// `arr.Insert(1, arr[1])` — the shift-up half of an insert, where the memmove
// runs BEFORE the copy, so the source slot is one the move has just relocated.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           insert_of_a_slot_of_the_same_array_balances_spec_28_9) {
    expectNoOverRelease(
        mutationProducer() +
        "class C public static method Main(): int "
        "  var arr := P.MakeDict().ToArray(); "
        "  arr.Insert(1, arr[1]); "
        "  Console.WriteLine($\"{arr.Count} {arr[0].Key} {arr[1].Key} {arr[2].Key}\"); "
        "  return 0; "
        "end Main; end C; ",
        "4 aa bb bb");
}

// A duplicate created first, then a REMOVE — the removal must give back exactly
// one of the two references the duplicate created, leaving the survivor's.
BEZEL_TEST(dict_entry_toarray_key_coown_6550,
           remove_at_after_a_duplicate_leaves_the_survivor_owned_spec_28_9) {
    expectNoOverRelease(
        mutationProducer() +
        "class C public static method Main(): int "
        "  var arr := P.MakeDict().ToArray(); "
        "  arr[0] := arr[1]; "
        "  arr.RemoveAt(0); "
        "  Console.WriteLine($\"{arr.Count} {arr[0].Key}\"); "
        "  return 0; "
        "end Main; end C; ",
        "2 bb");
}

}  // namespace
