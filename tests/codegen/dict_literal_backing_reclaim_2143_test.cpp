// Codegen — issue #2143 (§28.9 runtime-managed / trivial-dispose types, §28.11
// borrowing collections, spec v0.220): a plain `var d: Dictionary<K,V> :=
// [k:v, …]` collection literal leaks its backing (the bz_dict_t table +
// entries + any string keys/values) at scope end.
//
// CORRECTED FINDING vs. the filed issue: #2143 as filed characterized this as
// an ASYNC-ONLY gap ("Repro A ... sync clean, async leaks"). Re-verifying on
// main c3ea38c with a single-shot probe reproduced that "sync clean" reading —
// but it is an LSan FALSE NEGATIVE, not real cleanliness: LeakSanitizer is a
// CONSERVATIVE stack scanner (it treats any stale pointer-shaped byte pattern
// still sitting in unused stack memory as a live root), and a single trivial
// `Main` call leaves just enough of a stale `bz_dict_t*` value on the stack to
// hide the leak. Wrapping the identical literal in a 30-iteration loop (so the
// leak accumulates far past what any one stale stack slot could mask) shows it
// leaking in SYNC too — 12360 B / 150 allocations for 30 iterations, ~412 B
// per iteration, matching the async repro's per-call leak size exactly. So
// this is a plain missing-reclaim gap present in EVERY context (sync, async,
// iterator, lambda) — not an async-frame-specific defect — and the fix (like
// #2142's Set-literal fix) is a single shared-pass extension, not a
// resume-fn-only patch. (probe evidence: /var/tmp/bezel-scratch/laneH/
// dict_literal_sync_loop.bzl, dict_literal_sync.bzl, dict_literal_async.bzl)
//
// Root cause: `Dictionary<K,V>` is trivial-dispose (§28.9/§7052) exactly like
// `Array<T>`/`Set<T>` — its backing must be reclaimed at scope end by an
// implementation-defined mechanism. A `[k:v, …]` literal (non-empty) lowers via
// its OWN dedicated early-return branch in `lowerLocalDecl`
// (`materializeHeapDictLiteral`) which stored the slot but registered NO
// disposal at all — not even the array-literal's `borrowingArrayFreeEligible_`
// gate (that requires name-head "Array"), nor the dict-MATERIALIZER gate
// (`dictMaterializerFreeEligible_`, which requires a `ToDictionary()`-style
// CALL initializer, not a literal). The EMPTY literal `[]` targeting a
// Dictionary takes a DIFFERENT branch (the ambiguous-empty-form dispatches
// through `materializeHeapCollectionLiteral`, §15.4 R39) — also unregistered.
//
// Fix: a new `isBorrowingDictLiteralLocal` recognizer + `dictLiteralFreeEligible_`
// eligibility set (populated by the same `collectBorrowingArrayFreeEligible`
// escape-walk pass used for #2142's Set-literal fix), with TWO registration
// sites (the empty-literal branch and the dedicated non-empty dict-literal
// branch) both emitting the reclaim via `buildCollectionTempDisposal` — the
// same type-driven helper that already derives the correct per-value disposer
// (null / string-release / record-box-free) for the call-argument temp-
// disposal path, reused here rather than duplicated.
//
// Repro B (call-returned borrowing Array<T> across an await) is analyzed but
// NOT fixed here — see this lane's final report for why it is a genuinely
// different, broader problem (the compiler cannot soundly assume a plain,
// non-`owned`-marked `Array<T>`-returning user method is fresh vs. an aliased
// borrow without either interprocedural analysis or a language-level `owned`
// marker convention) and the corrected finding that it ALSO leaks in sync.
//
// fail-without/pass-with (runAotChecked). // per §28.9, §28.11 (issue #2143,
// spec v0.220)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
// For the two escape/no-double-free controls below: assert VALUE correctness
// only (no ASan/LSan instrumentation). Once a Dictionary literal ESCAPES (via
// `return` or a field store) it is, BY DESIGN, excluded from
// dictLiteralFreeEligible_ — its backing is then never reclaimed by ANY
// mechanism (a separate, out-of-scope leak — the call-returned/aliased-
// collection family #2143's own repro B belongs to). Using runAotChecked
// there would fail on that EXPECTED, unrelated leak and mask the actual
// point of the test (no UAF / no double-free — the value must still be
// correct). runAot skips ASan entirely, so only a genuine crash (SEGV/UB)
// flips `ran` to false.
int runValueOnly(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The issue's exact repro (sync), LOOPED 20x with small per-entry values
// (so the summed total fits the 8-bit process exit code) to defeat the LSan
// stale-stack false negative documented above — each iteration constructs-
// and-drops a fresh Dictionary<string,int> literal; without the fix the
// backing accumulates unfreed across all 20 iterations (verified: 8240 B / 100
// allocations leaked on main c3ea38c). NOTE: a 30-iteration loop with indexer
// reads trips an UNRELATED, pre-existing crash (confirmed present with ZERO of
// this fix's code, via a `using var := new Dictionary<string,int>()` +
// indexer-set/get control at 30 iterations — filed separately, see this
// lane's final report); 20 iterations stays clear of it while remaining far
// past the single-shot false-negative's masking margin. Value: sum of (a+b)
// per iteration = 20 * (1 + 2) = 60. // per §28.9, §28.11 (issue #2143)
BEZEL_TEST(dict_literal_backing_reclaim_2143, sync_string_key_dict_literal_looped_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..20 do "
        "      var d: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
        "      total := total + d[\"a\"] + d[\"b\"]; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        60);
}

// ── The issue's exact repro, ASYNC form (held across an await). This single-
// shot form reliably shows the leak WITHOUT looping (verified: 412 B / 5
// allocations against main c3ea38c) — the async resume function's stack
// layout does not exhibit the sync single-shot's stale-pointer masking.
// Value: Count = 2. // per §28.9, §28.11, §30.2 (issue #2143)
BEZEL_TEST(dict_literal_backing_reclaim_2143, async_string_key_dict_literal_across_await_spec_28_9_30_2) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var d: Dictionary<string, int> := [\"a\": 10, \"b\": 20]; "
        "    await Task.Delay(30); "
        "    return d.Count; "
        "  end Main; "
        "end C;"),
        2);
}

// ── EDGE: the empty literal `Dictionary<string,int> := []` (§15.4 R39 —
// parses with isDict=false, routed through the OTHER lowerLocalDecl branch
// than the non-empty case above; both branches needed the fix). Value: 0.
// per §15.4, §28.9 (issue #2143)
BEZEL_TEST(dict_literal_backing_reclaim_2143, empty_dict_literal_edge_spec_15_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var d: Dictionary<string, int> := []; "
        "    return d.Count; "
        "  end Main; "
        "end C;"),
        0);
}

// ── EDGE: a single-entry literal. Value: Count = 1.
// per §28.9 (issue #2143)
BEZEL_TEST(dict_literal_backing_reclaim_2143, single_entry_dict_literal_edge) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var d: Dictionary<string, int> := [\"only\": 7]; "
        "    return d[\"only\"]; "
        "  end Main; "
        "end C;"),
        7);
}

// ── INT-KEY axis: Dictionary<int,int> (the generic-key `bz_dict_alloc_generic`
// path, like Set<int> — distinct runtime entry point from the string-key fast
// path `bz_dict_alloc` the tests above exercise). Value: Count = 3.
// per §28.9, §4.8 (issue #2143)
BEZEL_TEST(dict_literal_backing_reclaim_2143, int_key_dict_literal_generic_path_spec_4_8) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var d: Dictionary<int, int> := [1: 10, 2: 20, 3: 30]; "
        "    return d.Count; "
        "  end Main; "
        "end C;"),
        3);
}

// ── STRING-VALUE axis: Dictionary<string,string> — the VALUE is runtime-
// managed (co-owned +1, stdlib §4.10/#429), a DIFFERENT disposer arm than the
// plain-int value above (dictValueDisposer = the string-release thunk, not
// null). Value: Count = 2. // per §28.9, §4.10 (issue #2143)
BEZEL_TEST(dict_literal_backing_reclaim_2143, string_value_dict_literal_spec_4_10) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var d: Dictionary<string, string> := [\"a\": \"x\", \"b\": \"y\"]; "
        "    return d.Count; "
        "  end Main; "
        "end C;"),
        2);
}

// ── RECORD-VALUE axis: Dictionary<string, Point> — Point is a value type
// (record), boxed per-value (dictValueBoxFreer: a plain `free` of the heap
// box, nothing to dispose — a record has no destructor). A THIRD, distinct
// disposer arm from the null/string-release cases above. Value: p.x + p.y +
// Count = 3 + 4 + 1 = 8. // per §28.9, §9 (issue #2143)
BEZEL_TEST(dict_literal_backing_reclaim_2143, record_value_dict_literal_box_free_spec_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Point(x: int, y: int); "
        "class C "
        "  public static method Main(): int "
        "    var d: Dictionary<string, Point> := [\"p\": new Point(3, 4)]; "
        "    var p := d[\"p\"]; "
        "    return p.x + p.y + d.Count; "
        "  end Main; "
        "end C;"),
        8);
}

// ── REVIEWER PRE-EMPT #1 control: does the scope-end reclaim double-free when
// the Dictionary ESCAPES via return? Same shape/reasoning as #2142's Set
// control (StringEscapeWalker is unmodified — only its candidate INPUT set is
// extended). `Make`'s own scope-end drop must be skipped (the return is
// classified as an escape); the caller's CallExpr-initialized `got` local is
// not matched by isBorrowingDictLiteralLocal either (not a literal
// initializer) — a separate, out-of-scope leak (the #2143 repro-B family),
// not a double-free. Confirmed leaking-but-not-crashing under ASan (a
// pre-existing, out-of-scope gap — hence runValueOnly, not runChecked: this
// test's SOLE point is that the value is correct, i.e. no UAF/double-free).
// Value: got.Count = 2. // per §28.9, §28.11 (issue #2143 self-pass)
BEZEL_TEST(dict_literal_backing_reclaim_2143, dict_literal_escape_via_return_no_double_free) {
    BEZEL_EXPECT_EQ(runValueOnly(
        "module M; "
        "class C "
        "  public static method Make(): owned Dictionary<string, int> "
        "    var d: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
        "    return d; "
        "  end Make; "
        "  public static method Main(): int "
        "    var got := C.Make(); "
        "    return got.Count; "
        "  end Main; "
        "end C;"),
        2);
}

// ── REVIEWER PRE-EMPT control: does the scope-end reclaim double-free when the
// Dictionary ESCAPES via a FIELD STORE? `c.bag := d;` must be classified as an
// escape, excluding `d` from dictLiteralFreeEligible_ in Main's scope — the
// field's later read must not be a UAF. Confirmed leaking-but-not-crashing
// under ASan (pre-existing, out-of-scope — see the return-escape control
// above); runValueOnly asserts the value only. Value: c.bag.Count = 2.
// per §28.9, §28.11 (issue #2143 self-pass)
BEZEL_TEST(dict_literal_backing_reclaim_2143, dict_literal_escape_via_field_store_no_double_free) {
    BEZEL_EXPECT_EQ(runValueOnly(
        "module M; "
        "class C "
        "  public bag: Dictionary<string, int> := []; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var d: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
        "    c.bag := d; "
        "    return c.bag.Count; "
        "  end Main; "
        "end C;"),
        2);
}

// ── ITERATION safety: `for var kv in d do ... end` must be a borrow-read (the
// per-loop enumerator borrows the table without retaining it), not an escape
// — so `d` stays eligible for the scope-end reclaim, and the free after the
// loop must not disturb the loop's own (already-disposed-per-iteration)
// enumerator. Value: total (10+20=30) + d.Count (2) = 32.
// per §28.9, §28.11, §29.1 (issue #2143 self-pass)
BEZEL_TEST(dict_literal_backing_reclaim_2143, dict_literal_iterate_then_read_no_uaf) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var d: Dictionary<string, int> := [\"a\": 10, \"b\": 20]; "
        "    var total := 0; "
        "    for var kv in d do "
        "      total := total + kv.Value; "
        "    end; "
        "    return total + d.Count; "
        "  end Main; "
        "end C;"),
        32);
}
