// Codegen — `.Aggregate` over a string-accumulator LINQ source leaks the
// references the fold itself produces. §28.9 (Runtime-managed types), §28.3
// (Local variables, ownership, and borrows), stdlib §5 `.Aggregate`, v0.338.
//
// THE DEFECT, IN TWO HALVES, AND WHY THE SECOND HALF IS THE ONE THAT
// IDENTIFIES IT.
//
// (1) Every value the fold produced but the LAST was overwritten in the
//     accumulator slot and dropped with its +1 intact — one string per element
//     per evaluation. The `.Aggregate` arm lowers the loop itself
//     (`tryLowerCollectionInstanceCall`, LlvmBackend_Calls.cpp), storing each
//     step's result into an `entryAlloca` accumulator with no discharge of the
//     value it displaced.
//
// (2) The LAST value — the one the arm returns — was classified a BORROW by
//     every consumer, so nothing ever adopted the fold's own +1 and nothing
//     discharged it either. One reference per EVALUATION.
//
// The distinction is measured, not assumed. Holding the per-step release in
// place and removing only the +1-producer recognition, the residual over 101
// iterations is 200 allocations for a ONE-, a TWO- and a THREE-element source
// alike — and 0 for an empty one, which folds nothing (`bezelc
// --sanitize=address`). An intermediate the loop dropped scales with the
// element count; this one does not. Only an element-count-invariant residue is
// the returned value.
//
// THE BOUND/UNBOUND ASYMMETRY. On the parent, a BOUND receiver
// (`var a := f.Make(); a.Aggregate(...)`) measured 7,235 B / 402 allocations
// and the UNBOUND twin (`f.Make().Aggregate(...)`) 25,819 B / 604. The
// 18,584 B difference is exactly the receiver portion, which the eager-terminal
// reclaim already takes; the residue is byte-identical on both sides. That is
// what identifies it as the fold's own output and NOT an unbound-receiver
// story: a bound receiver is discharged by its own owner, so nothing bound can
// leak this way.
//
// SCOPE — WHAT THESE ROWS DO NOT CLAIM. The UNBOUND receiver over an
// `Array<string>` still measures 18,400 B after this fix, and that is a
// DIFFERENT defect, not a residue of this one: it is the receiver's own table,
// which the element-returning-terminal discipline (#6393) declines to reclaim
// on a `string`-element source because the fold's result aliases one of its
// slots. Pinned separately; not pinned here, and not asserted clean here.
//
// THE IDENTITY FOLD IS THE SOUNDNESS ROW, not a curiosity. `lambda(x, y) => x`
// returns its accumulator parameter — the one shape where an over-eager
// per-step release would free something the loop does not own. It measures
// CLEAN under ASan, and a NON-SENTINEL (`"p" + "q"`) seed is used for it so a
// release imbalance cannot hide behind a literal's immortal reference count.
// ASan reports neither a use-after-free nor a double free, which is what
// establishes that the closure's return path really does mint a fresh reference
// for whatever the fold returns — the fact the +1-producer recognition rests
// on.
//
// THE NULLABLE AXIS — AND WHY EVERY ROW ABOVE WAS BLIND TO IT. "The closure's
// return path mints" holds for a NON-NULLABLE accumulator only. A `string?`
// returning lambda deliberately does NOT mint (#2688, and its ReturnStmt
// sibling says so in terms: "a `string?` lambda return does not mint there, so
// it must not mint here either… the method-side `string?` mint (#2196) is a
// method-contract rule the lambda call-site ABI does not share"). Both body
// forms gate the mint on `!retSemaTy->nullable`. So on a nullable accumulator
// the fold hands its input back unchanged, the loop owns NO reference, and the
// per-step and seed discharges over-release.
//
// The transfer half is read off `exprBody` OR, for a block body, off a
// single-return `ReturnStmt` — the same two places the mint sites read. The first version of this fix
// assumed otherwise and read the nullable shape as admitted on the strength of
// `bz_string_release` being null-safe — which says nothing about the failing
// case, because the accumulator that fails is NOT null.
//
// MEASURED, as an 8-cell toggle of the three sub-changes (`bezelc
// --sanitize=address`, 101 iterations, 6-element `Array<string>`, seed
// `"n" + "ull"`, identity fold `lambda(x: string?, y: string): string? => x`):
//
//   per-step  seed  producer-recog   result
//     –       –          –             3700 B / 200 allocs   (the residue)
//     –       –          ✓             CLEAN                 <-- the recog alone
//     –       ✓          –/✓           heap-use-after-free
//     ✓       –          –/✓           over-release, exit 134
//     ✓       ✓          –/✓           over-release, exit 134
//
// EITHER discharge over-releases on its own, so the release sites — not the
// recognition — are what carry the `!nullable` gate. And the gate is NOT the
// blanket `!nullable` it looks like: the mint is a transfer-or-retain, so the
// discharge is sound whenever EITHER half fires. A nullable CONCATENATING fold
// (`=> x! + y`) takes the transfer half and is clean at the rejected revision,
// so dropping it wholesale costs 25,452 B / 1,212 allocations. That is what
// `nullable_accumulator_concatenating_fold_still_releases_each_step` pins: it
// is red for a blanket `!nullable` and green for the mint rule, and it is the
// row that separates this fix from the band-aid it could have been.
//
// A leak pin must prove its detector ran. `runAotChecked` defaults to
// `ASAN_OPTIONS=detect_leaks=0`, so a plain row is VACUOUS for a leak: green on
// a broken tree while showing a clean sanitizer trio. Every row arms the leak
// oracle with `BEZEL_AOT_DETECT_LEAKS=strict` (save/restored) and asserts
// `!checkedToolchainSkipped(r)` BEFORE any guard that could absorb the skip.
// No `LSAN_OPTIONS`: these are found at LSan defaults.

#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

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

bool skipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// A BOUND `owned Array<string>` producer. The receiver is bound at the call
// site (`var a := P.Make();`) precisely so these rows measure the FOLD's
// references and nothing else — see the header.
std::string boundFold(const std::string& elements,
                      const std::string& seed,
                      const std::string& foldBody) {
    return
        "module M; "
        "class P public "
        "  static method Make(): owned Array<string> "
        "    return " + elements + "; "
        "  end Make; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while i < 101 do "
        "      var a := P.Make(); "
        "      var s := a.Aggregate(" + seed + ", " + foldBody + "); "
        "      n := n + s.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C; ";
}

void expectClean(const std::string& module) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(module);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

}  // namespace

// ===========================================================================
// THE CORE SHAPE. Two elements, a concatenating fold: before the fix, 402
// allocations over 101 iterations — two dropped intermediates plus the
// returned value's un-discharged reference, per evaluation.
// ===========================================================================
BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           two_element_string_fold_releases_every_reference_it_mints) {
    expectClean(boundFold("[\"aa\", \"bb\"]", "\"\"",
                          "lambda(x: string, y: string): string => x + y"));
}

// ===========================================================================
// THE ELEMENT-COUNT AXIS. A one-element source has NO intermediate to drop, so
// the per-step release alone cannot account for its cleanliness; a three-element
// source has two. Both must be clean, and the pair is what separates "the
// returned value's reference" (invariant in the element count) from "a dropped
// intermediate" (linear in it).
// ===========================================================================
BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           one_element_string_fold_releases_the_value_it_returns) {
    expectClean(boundFold("[\"aa\"]", "\"\"",
                          "lambda(x: string, y: string): string => x + y"));
}

BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           three_element_string_fold_releases_every_intermediate) {
    expectClean(boundFold("[\"aa\", \"bb\", \"cc\"]", "\"\"",
                          "lambda(x: string, y: string): string => x + y"));
}

// ===========================================================================
// THE SOUNDNESS ROW. The identity fold returns its accumulator parameter, the
// one value shape where discharging each step could over-release. Seeded with a
// NON-SENTINEL `+`-produced string so the imbalance cannot hide behind a
// literal's immortal reference count: a release that outran the fold's mint
// would show up here as a use-after-free or a double free under ASan.
//
// The seed is an inline `+` expression, not a local bound to one, and the
// distinction is measured rather than stylistic. The `.Aggregate` arm lowers
// its own seed argument, and a seed that is a +1 PRODUCER owes a discharge that
// nothing else reaches. Passed as an INLINE producer that obligation is
// unambiguous and is discharged (3500 B / 200 allocations on the parent, 0
// after this fix). Bound to a local first, the same seed measures 3535 B / 202
// on BOTH sides of this fix — the leaked object is the local's own string (32 B
// x 101), i.e. an argument-reference mechanism distinct from the fold's output
// and not claimed here. See the commit body; `seed_borrow_local_residual` in
// the handback carries the repro.
// ===========================================================================
BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           identity_fold_over_a_non_sentinel_inline_seed_does_not_over_release) {
    expectClean(
        "module M; "
        "class P public "
        "  static method Make(): owned Array<string> "
        "    return [\"aa\", \"bb\"]; "
        "  end Make; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while i < 101 do "
        "      var a := P.Make(); "
        "      var s := a.Aggregate(\"p\" + \"q\", "
        "          lambda(x: string, y: string): string => x); "
        "      n := n + s.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C; ");
}

// The seed is returned un-folded only when the source is EMPTY, so the seed's
// own reference is the RESULT's reference there and must not be discharged by
// the per-step rule. A discharge that ignored that would show here as a
// use-after-free on the very next read.
BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           empty_source_returns_the_seed_without_discharging_it) {
    expectClean(
        "module M; "
        "class P public "
        "  static method Make(): owned Array<string> "
        "    return []; "
        "  end Make; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while i < 101 do "
        "      var a := P.Make(); "
        "      var s := a.Aggregate(\"p\" + \"q\", "
        "          lambda(x: string, y: string): string => x); "
        "      n := n + s.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C; ");
}

// The same row without the sentinel seed, for the ordinary literal-seed case.
BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           identity_fold_over_a_literal_seed_is_clean) {
    expectClean(boundFold("[\"aa\", \"bb\"]", "\"zz\"",
                          "lambda(x: string, y: string): string => x"));
}

// ===========================================================================
// THE VALUE-ACCUMULATOR CONTROL. An `int` accumulator carries no reference, so
// the fix must emit neither the discharge slot nor the release for it. A
// regression that released an `int` accumulator's bits as a pointer would be a
// wild free, and this row is what sees it.
// ===========================================================================
BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           int_accumulator_fold_is_unchanged) {
    expectClean(
        "module M; "
        "class P public "
        "  static method Make(): owned Array<string> "
        "    return [\"aa\", \"bb\"]; "
        "  end Make; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while i < 101 do "
        "      var a := P.Make(); "
        "      var t := a.Aggregate(0, "
        "          lambda(x: int, y: string): int => x + y.Length); "
        "      n := n + t; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C; ");
}
// ===========================================================================
// THE NULLABLE AXIS. The seed's own expression type is NOT the axis — see the
// header. `nullableFold` below annotates the FOLD's return type `string?`,
// which is where the nullability lives and what the mint sites key on.
// ===========================================================================
namespace {

// A bound `owned Array<string>` producer folded with an ANNOTATED `string?`
// accumulator. `foldBody` is the lambda's body expression, `seed` its seed —
// both are interpolated into the same loop the other rows use, so the nullable
// rows differ from the non-nullable ones in that one axis and nothing else.
std::string nullableFold(const std::string& elements,
                         const std::string& seed,
                         const std::string& foldBody) {
    return
        "module M; "
        "class P public "
        "  static method Make(): owned Array<string> "
        "    return " + elements + "; "
        "  end Make; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while i < 101 do "
        "      var a := P.Make(); "
        "      var s := a.Aggregate(" + seed + ", " + foldBody + "); "
        "      n := n + s.Length; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C; ";
}

// The failing shape, on a 6-element source. The seed is an INLINE `+` producer
// so the loop's own seed discharge has something real to release, and the fold
// is the IDENTITY, so the value the arm returns IS the seed's reference: a
// loop that releases it frees the value it is about to hand back. Measured on
// the first (rejected) version of this fix: `fatal: refcount over-release
// (string) — two releases for one reference; terminating per §28.9`, exit 134.
// Every assertion below is the CORRECT post-fix behaviour — compiles, exit 0,
// no sanitizer report, no leak — so the row is not a pin on the defect and
// does not invert when the defect is fixed.
BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           nullable_accumulator_identity_fold_does_not_over_release) {
    expectClean(nullableFold(
        "[\"aa\", \"bb\", \"cc\", \"dd\", \"ee\", \"ff\"]", "\"n\" + \"ull\"",
        "lambda(x: string?, y: string): string? => x"));
}

// The one-element twin, and the row that pins the OTHER failure mode. With a
// single element there is no intermediate at all, so the residue the per-step
// discharge is there to take is zero and the seed discharge stands alone: at
// the rejected revision this measured `heap-use-after-free` under ASan (the
// seed freed while the arm still hands it back). Same program, one fewer
// element, a different sanitizer verdict — which is why both are pinned.
BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           nullable_one_element_identity_fold_does_not_use_after_free) {
    expectClean(nullableFold(
        "[\"aa\"]", "\"n\" + \"ull\"",
        "lambda(x: string?, y: string): string? => x"));
}

// THE BAND-AID ROW. A nullable accumulator whose fold is itself a +1 producer
// (`x! + y`) DOES mint a fresh reference per step, so the per-step discharge is
// correct and load-bearing here even though the accumulator is nullable. This
// row is green with the mint-rule gate and RED under the blanket `!nullable`
// that "just exclude nullable" would install: measured, 25,452 bytes in 1,212
// allocations over 101 iterations (`bezelc --sanitize=address`). It is the row
// that keeps the fix from being narrowed to the shape it was written against.
BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           nullable_accumulator_concatenating_fold_still_releases_each_step) {
    expectClean(nullableFold(
        "[\"aa\", \"bb\", \"cc\", \"dd\", \"ee\", \"ff\"]", "\"n\" + \"ull\"",
        "lambda(x: string?, y: string): string? => x! + y"));
}

// The same concatenating fold over an EMPTY source: no fold step runs, so the
// seed is the returned value and must survive undischarged. This is the
// nullable counterpart of `empty_source_returns_the_seed_without_discharging_it`,
// and it is where a `seedSlot` that re-armed wrongly would show.
BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           nullable_empty_source_returns_the_seed_without_discharging_it) {
    expectClean(nullableFold(
        "[]", "\"n\" + \"ull\"",
        "lambda(x: string?, y: string): string? => x! + y"));
}

// A nullable accumulator seeded with a BORROW (a plain literal). No discharge
// is owed for it in either direction, and the arm's `isPlusOneStringProducer`
// recognition must decline it exactly as it does for the non-nullable literal
// seed — so the consumer keeps its own reference and nothing is over-released.
BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           nullable_accumulator_identity_fold_over_a_literal_seed_is_clean) {
    expectClean(nullableFold(
        "[\"aa\", \"bb\"]", "\"zz\"",
        "lambda(x: string?, y: string): string? => x"));
}

}  // namespace

// A BLOCK-bodied fold reaches the same transfer half through its ReturnStmt
// instead of `exprBody` — which is what the ReturnStmt mint site reads. The
// single-return form is the only one decidable from the call site, and this
// row is what keeps it decided: reading only `exprBody` leaves it at 25,452 B
// / 1,212 allocations over 101 iterations (measured), against 30,352 B / 1,412
// on the pre-#6395 base, so the miss is under-discharge on both sides and never
// unsoundness — but it is a miss, and it is the same miss the expression-bodied
// twin does not make.
BEZEL_TEST(aggregate_string_accumulator_discharge_6395,
           nullable_block_bodied_concatenating_fold_releases_each_step) {
    expectClean(nullableFold(
        "[\"aa\", \"bb\", \"cc\", \"dd\", \"ee\", \"ff\"]", "\"n\" + \"ull\"",
        "lambda(x: string?, y: string): string? => do "
        "return x! + y; "
        "end"));
}
