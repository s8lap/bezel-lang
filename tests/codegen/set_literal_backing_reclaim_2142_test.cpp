// Codegen — issue #2142 (§28.9 runtime-managed / trivial-dispose types, §28.11
// borrowing collections, spec v0.220): a plain `var s: Set<T> := [ ... ]`
// collection literal leaks its backing (the bz_dict_t table + entries + any
// string keys) at scope end — in BOTH a synchronous and an asynchronous body.
//
// Root cause (confirmed by reading LlvmBackend_Setup.cpp before the fix):
// `Set<T>` is a borrowing-family collection exactly like `Array<T>` (spec
// table, §28.9/§7052's row: "Array<T> and other borrowing collections ...
// Backing storage is reclaimed with the instance by the implementation-defined
// mechanism; Dispose() runs no user code"). A `[...]` literal targeting
// `Set<T>` materializes via `materializeHeapCollectionLiteral`, which
// dispatches internally to `materializeHeapSetLiteral` (a `bz_dict_t*`, NOT a
// `bz_array_t*`) — but the SAME early-return branch's scope-end reclaim
// registration was gated on `borrowingArrayFreeEligible_`, whose membership
// test (`isBorrowingArrayLiteralLocal`) requires the resolved type's name-head
// to be exactly "Array". A Set<T> local therefore matched NEITHER that set
// NOR the dict-materializer set (`dictMaterializerFreeEligible_`, which
// requires a `ToDictionary()`-style CALL initializer, not a list literal) — it
// was in NEITHER eligibility set, so no scope-end free was ever emitted, sync
// or async.
//
// Fix: a new `isBorrowingSetLiteralLocal` recognizer + `setLiteralFreeEligible_`
// eligibility set (populated by the SAME `collectBorrowingArrayFreeEligible`
// escape-walk pass, in `borrowingArrayMode` exactly like the Array-literal
// pass — a `for var x in s` iterates via a per-loop enumerator that borrows
// the table without retaining it, so it is a borrow-read, not an escape), plus
// a new registration site in `lowerLocalDecl`'s collection-literal branch that
// emits the reclaim via `buildCollectionTempDisposal` (the SAME type-driven
// Set/Dict dispatch already used for call-argument temp disposal — handles
// the `Set<string>` co-owned +1 key release via `dictStringKeys` correctly,
// reusing existing logic rather than duplicating it). Because the eligibility
// set is computed by the SAME shared pass #2132 already re-establishes for the
// async RESUME fn (`LlvmBackend_Async.cpp`) and the iterator MOVENEXT fn
// (`LlvmBackend_Iterators.cpp`, #2148), extending that one pass automatically
// closes the leak in every context those fixes already cover — sync entry,
// async resume, async lambda resume, and iterator MoveNext — with no separate
// per-context patch needed.
//
// fail-without/pass-with (runAotChecked): without the fix every non-escaping
// Set<T> literal leaks (LSan fires, `r.ran` -> false, mapped to the -999
// sentinel below); with the fix every program is ASan/LSan-clean and returns
// the expected value. // per §28.9, §28.11 (issue #2142, spec v0.220)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// See async_array_frame_spill_2132_test.cpp's `runChecked`: -999 sentinel on
// any sanitizer fire (leak / UAF / double-free), else the real exit code — one
// EXPECT_EQ pins both memory-cleanliness and the computed value.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
// For the two escape/no-double-free controls below: assert VALUE correctness
// only (no ASan/LSan instrumentation). A Set literal that ESCAPES (via
// `return` or a field store) is, BY DESIGN, excluded from
// setLiteralFreeEligible_ — its backing is then never reclaimed by ANY
// mechanism (a separate, pre-existing, out-of-scope leak — the call-returned/
// aliased-collection family, same shape as #2143's repro B). runAotChecked
// would fail on that EXPECTED, unrelated leak and mask the actual point of
// the test (no UAF / no double-free); runAot skips ASan, so only a genuine
// crash flips `ran` to false.
int runValueOnly(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The issue's exact repro (sync). Set<int> := [10, 20, 30]; return Count.
// Without the fix: 420 B leaked in 6 allocations (verified against main
// c3ea38c under `--sanitize=address`). Value: Count = 3.
// per §28.9, §28.11 (issue #2142)
BEZEL_TEST(set_literal_backing_reclaim_2142, sync_int_set_literal_spec_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var s: Set<int> := [10, 20, 30]; "
        "    return s.Count; "
        "  end Main; "
        "end C;"),
        3);
}

// ── The issue's exact repro, ASYNC form (held across an await — the same
// eligibility set must be re-established for the resume fn, mirroring #2132).
// Without the fix: leaks identically (420 B / 6 allocations — confirmed the
// same as sync; this is NOT the #2132 frame-spill defect, which was fixed
// separately and already covers Set's frame round-trip generically).
// Value: Count = 3. // per §28.9, §28.11, §30.2 (issue #2142)
BEZEL_TEST(set_literal_backing_reclaim_2142, async_int_set_literal_across_await_spec_28_9_30_2) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var s: Set<int> := [10, 20, 30]; "
        "    await Task.Delay(30); "
        "    return s.Count; "
        "  end Main; "
        "end C;"),
        3);
}

// ── STRING-ELEMENT axis: Set<string>. Elements are stored as KEYS in the
// generic-key table and co-own a +1 (stdlib §4.10/#429) — the reclaim must
// release that +1 via `bz_set_string_keys_release` (dictStringKeys=true)
// BEFORE `bz_dict_free`, or every string element leaks its own retain in
// addition to the table. Distinct disposer arm from the plain-int case above.
// Value: Count = 3. // per §28.9, §28.11, §4.10 (issue #2142)
BEZEL_TEST(set_literal_backing_reclaim_2142, sync_string_set_literal_string_keys_spec_4_10) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var s: Set<string> := [\"a\", \"b\", \"c\"]; "
        "    return s.Count; "
        "  end Main; "
        "end C;"),
        3);
}

// ── EDGE: the empty literal `Set<int> := []`. §15.4 admits the empty form for
// every collection target; materializeHeapSetLiteral's insertion loop is then
// a no-op over zero elements, but the table itself (header + buckets) is still
// a real heap allocation that must be reclaimed. Value: Count = 0.
// per §15.4, §28.9 (issue #2142)
BEZEL_TEST(set_literal_backing_reclaim_2142, empty_set_literal_edge_spec_15_4) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var s: Set<int> := []; "
        "    return s.Count; "
        "  end Main; "
        "end C;"),
        0);
}

// ── EDGE: a single-element literal `Set<int> := [42]`. Value: Count = 1.
// per §28.9 (issue #2142)
BEZEL_TEST(set_literal_backing_reclaim_2142, single_element_set_literal_edge) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var s: Set<int> := [42]; "
        "    return s.Count; "
        "  end Main; "
        "end C;"),
        1);
}

// ── Nested: a Set<int> literal declared inside an async LAMBDA body and held
// across the lambda's own await — mirrors #2132's
// `borrowing_array_literal_in_async_lambda`. emitAsyncLambda's resume setup
// re-establishes ALL FIVE eligibility sets (array/dict-materializer/grouping/
// set-literal/dict-literal) via the single shared
// collectBorrowingArrayFreeEligible call, so this exercises that shared wiring
// for the Set-literal set specifically. Value: 3.
// per §30.2, §28.9, §28.11 (issue #2142)
BEZEL_TEST(set_literal_backing_reclaim_2142, set_literal_in_async_lambda_spec_30_2) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f := async lambda(): Task<int> => do "
        "      var s: Set<int> := [10, 20, 30]; "
        "      await Task.Delay(30); "
        "      return s.Count; "
        "    end; "
        "    return await f(); "
        "  end Main; "
        "end C;"),
        3);
}

// ── REVIEWER PRE-EMPT #1 control: does the scope-end reclaim double-free when
// the Set ESCAPES via return? `Make()` returns the literal directly (a borrow
// of a local about to go out of Make's scope — the same shape as the existing
// `Array<T>` precedent, e.g. bug_465_test.cpp's `return_array_int_literal_465`).
// The escape walk (StringEscapeWalker, unmodified — only its CANDIDATE INPUT
// set is extended by this fix) must classify `return s;` as an escape (not a
// safe borrow-position read), excluding it from setLiteralFreeEligible_ in
// Make's OWN scope — so Make does NOT free it before handing it to the caller.
// The caller's own `var got := C.Make();` binding is a CallExpr initializer
// (not a CollectionLiteralExpr), so isBorrowingSetLiteralLocal does not match
// it either — `got` is simply not reclaimed by this mechanism (a pre-existing,
// separate leak — the call-returned-collection gap the issue text itself flags
// as its own family, #2143's repro B; OUT OF SCOPE here). This test only pins
// that returning the literal is NOT a double-free / UAF: Make's own scope-end
// drop must be skipped, and the caller must still read a valid value.
// Confirmed leaking-but-not-crashing under ASan (420 B / 6 allocations,
// verified) — hence runValueOnly, not runChecked: this test's sole point is
// that the value is correct. Value: got.Count = 3.
// per §28.9, §28.11 (issue #2142 self-pass)
BEZEL_TEST(set_literal_backing_reclaim_2142, set_literal_escape_via_return_no_double_free) {
    BEZEL_EXPECT_EQ(runValueOnly(
        "module M; "
        "class C "
        "  public static method Make(): owned Set<int> "
        "    var s: Set<int> := [10, 20, 30]; "
        "    return s; "
        "  end Make; "
        "  public static method Main(): int "
        "    var got := C.Make(); "
        "    return got.Count; "
        "  end Main; "
        "end C;"),
        3);
}

// ── REVIEWER PRE-EMPT control: does the scope-end reclaim double-free when the
// Set ESCAPES via a FIELD STORE? `c.bag := s;` binds the literal's backing
// into a class field that outlives Main's scope. The escape walk must treat
// this store as an escape (not a safe borrow-position read) and exclude `s`
// from setLiteralFreeEligible_ — so Main's scope-end drop is skipped and the
// field's later read is not a UAF. Confirmed leaking-but-not-crashing under
// ASan (pre-existing, out-of-scope — see the return-escape control above);
// runValueOnly asserts the value only. Value: c.bag.Count = 3.
// per §28.9, §28.11 (issue #2142 self-pass)
BEZEL_TEST(set_literal_backing_reclaim_2142, set_literal_escape_via_field_store_no_double_free) {
    BEZEL_EXPECT_EQ(runValueOnly(
        "module M; "
        "class C "
        "  public bag: Set<int> := []; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var s: Set<int> := [10, 20, 30]; "
        "    c.bag := s; "
        "    return c.bag.Count; "
        "  end Main; "
        "end C;"),
        3);
}

// ── ITERATION safety: `for var x in s do ... end` must be treated as a
// borrow-read (per-loop enumerator over the dict table, no retain) — not an
// escape — so `s` STAYS eligible for the scope-end reclaim, and the free that
// then runs after the loop must not have freed anything the loop's own
// enumerator still needed (the enumerator's own Dispose runs at the END of
// each loop iteration/exit, strictly before the outer scope-end reclaim final
// use). Value: total (10+20+30=60) + s.Count (3) = 63.
// per §28.9, §28.11, §29.1 (issue #2142 self-pass)
BEZEL_TEST(set_literal_backing_reclaim_2142, set_literal_iterate_then_read_no_uaf) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var s: Set<int> := [10, 20, 30]; "
        "    var total := 0; "
        "    for var x in s do "
        "      total := total + x; "
        "    end; "
        "    return total + s.Count; "
        "  end Main; "
        "end C;"),
        63);
}
