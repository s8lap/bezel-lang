// Codegen / leak — a string INDEX or SLICE whose receiver is a PRODUCER
// expression never discharged that receiver. Issue #7171.
//
// Spec: language spec §28.3 (*Unbound owned temporaries* — "a member or index
// access is one of the borrowing uses of an unbound owned temporary, and such
// a temporary is disposed at the END of the FULL EXPRESSION in which it is
// produced, after every use of it within that expression has completed"),
// §4.6 (`s[i]` returns the code point at `i`; `s[r]` returns "a fresh
// `string`" spanning `r` — neither transfers ownership of the receiver), and
// §28.9 (runtime-managed `string` values are released with `bz_string_release`).
// Spec revision v0.343.
//
// THE BUG. `src/codegen/LlvmBackend_Exprs.cpp`'s `ast::IndexExpr` arm (the
// string branch) lowered the receiver with `lowerExpr(*ix->target)` and
// returned the index / slice result with NO discharge registered:
//
//   * the char-index path:      `return loadCodePoint(strPtr, idx64);`
//   * the range-slice path:     `return emitStringRangeSlice(strPtr, *range);`
//   * the range-value path:     `return emitStringRangeSliceValue(strPtr, rv);`
//
// ⇒ the receiver — a fresh runtime-allocated `string` — was never released and
// leaked for the whole program.
//
// THE ASYMMETRY THAT LOCALIZES IT (MEASURED on this lane's base e0a44e9c with
// `bezelc --sanitize=address` and `ASAN_OPTIONS=detect_leaks=1`):
//
//   var c: char := (k + "cd")[0];        -> 36 byte(s) leaked in 2 allocation(s)
//   var t: string := (k + "cd")[0..3];    -> 37 byte(s) leaked in 2 allocation(s)
//   var r := 0..3; (k + "cd")[r];         -> 37 byte(s) leaked in 2 allocation(s)
//   (k + "cd")[0..999]  (caught)         -> 37 byte(s) leaked in 2 allocation(s)
//   Console.WriteLine((k + "cd").Length) -> clean
//   var s := k + "cd"; var c := s[0];    -> clean
//
// ⇒ the MEMBER-ACCESS form releases the temporary and the INDEX forms do not;
// the BOUND-receiver form is clean because the local's own scope-end disposal
// covers it. The defect is specifically the UNBOUND TEMPORARY receiver in
// index / slice position.
//
// THE LEAKED OBJECT IS THE RECEIVER, NOT THE RESULT. The byte count tracks the
// receiver's length (a 32-byte `bz_string` header plus the receiver's code
// points and their NUL terminator), not the result's — a 2-code-point
// receiver leaks 35 B and a 4-code-point receiver leaks 37 B, while the result
// is 2 B in both.
//
// THE FIX. The string index / slice path now registers the §28.3 receiver
// discharge — the string twin of the discharge the COLLECTION indexer path
// already registers (`src/codegen/LlvmBackend_Assign.cpp`'s
// `lowerCollectionIndexerGet`, ~line 706: `disposeStack_.back().push_back(d)`).
// It is a SCOPE-END REGISTRATION, not an inline free: the index result is read
// OUT of the receiver (the char path loads the code point from its bytes; the
// slice path COPIES them), so an inline free would free the very header the
// next instruction loads. The registration happens AT THE PRODUCTION — before
// the bounds check can raise — so the `IndexOutOfRangeException` edge is
// covered too.
//
// OBSERVABILITY. `runAotChecked` compiles, links and RUNS the generated code
// under ASan/LSan; a leaked string flips `r.ran` to false. Every case renders
// in a LOOP (60 iterations) so the per-iteration leak is unambiguously visible
// to LSan (a one-shot leak can linger in a register and evade the
// reachability scan). The returned count == the iteration count IFF every
// render is byte-correct AND the run is leak-clean, so the exit code pins the
// VALUE and `r.ran` pins leak-freedom.
//
// ⚠ THE THROW EDGE NEEDS ITS OWN ARM, AND IT MUST CATCH. The runtime's
// unhandled-exception exit path uses `_exit(70)`, which BYPASSES LSan's
// atexit handler — so a probe that lets the exception escape under-reports
// (no leak is ever reported) and a reviewer would conclude "no leak". Every
// throwing row below therefore CATCHES the exception and returns normally, so
// LSan actually runs.
//
// NOT IN SCOPE (kept separate from this issue on purpose):
//   * #7159 — the THROW-EDGE class for string METHODS that raise; this issue
//     is the NEVER-RELEASED class.
//   * #7156 — a SEMA admission gap on a member-call shape.
//   * #6614 — the range-slice carve-out in the COLLECTION path (an `Array<T>`
//     receiver, a different file and a different receiver type).
//   * #6480 — the §28.3 indexer-get receiver discharge in an EXPRESSION-BODIED
//     LAMBDA, which has no scope to register into. Every repro here is an
//     ordinary method body, where `disposeStack_` is available.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The shared scaffolding: a 60-iteration loop that counts byte-correct
// renders. The count is 60 (<= 255) so the 8-bit exit status carries it
// untruncated. `body` is the loop body's statements.
std::string loop60(const char* pre, const char* body) {
    return std::string(
               "module M; class C "
               "  public static method Main(): int ") +
           pre +
           "    var count := 0; "
           "    var i := 0; "
           "    while i < 60 do " + body +
           "      i := i + 1; "
           "    end; "
           "    return count; "
           "  end Main; "
           "end C;";
}

// The producer receiver under test. `k` is a BOUND local (so it is not itself
// a producer); `k + "cd"` is a string concat, which §28.9 classifies as a
// codegen-internal +1 producer — the shape whose reference the index read
// used to drop on the floor.
constexpr const char* kProducer = "var k := \"ab\"; ";

}  // namespace

// =====================================================================
// (A) The CHAR INDEX over a producer receiver: `(k + "cd")[0]` (§4.6).
// The index is a BORROWING use of the temporary — the result is a `char`,
// a value that cannot alias the receiver — so the receiver's reference dies
// at the end of the full expression. Pre-fix: 36 B in 2 allocations per
// iteration (2160 B over the loop).
// =====================================================================
BEZEL_TEST(string_index_producer_receiver_discharge_7171,
           char_index_over_producer_receiver_lsan_clean_spec_4_6) {
    JitResult r = runAotChecked(loop60(
        kProducer,
        "  var c: char := (k + \"cd\")[0]; "
        "  if c = 'a' then count := count + 1; end; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);   // LSan-clean: the receiver's +1 was released
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// =====================================================================
// (B) The RANGE SLICE over a producer receiver — the literal spelling
// `(k + "cd")[0..3]` (§3.4 / §4.6: the result is a FRESH `string`).
// Pre-fix: 37 B in 2 allocations per iteration.
// =====================================================================
BEZEL_TEST(string_index_producer_receiver_discharge_7171,
           range_slice_over_producer_receiver_lsan_clean_spec_4_6) {
    JitResult r = runAotChecked(loop60(
        kProducer,
        "  var t: string := (k + \"cd\")[0..3]; "
        "  if t.Length = 4 then count := count + 1; end; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// =====================================================================
// (C) The RANGE-VALUE spelling — the index is a Range-typed VALUE, not a
// literal: `var r := 0..3; (k + "cd")[r]` (§4.6 / stdlib §9.6's
// `public pure readonly property [r: Range<int>]: string`). The two
// spellings mint the same rc := 1 production, so both must discharge the
// receiver. Pre-fix: 37 B in 2 allocations per iteration.
// =====================================================================
BEZEL_TEST(string_index_producer_receiver_discharge_7171,
           range_value_slice_over_producer_receiver_lsan_clean_spec_4_6) {
    JitResult r = runAotChecked(loop60(
        "var k := \"ab\"; var r := 0..3; ",
        "  var t: string := (k + \"cd\")[r]; "
        "  if t.Length = 4 then count := count + 1; end; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// =====================================================================
// (D) THE THROW EDGE — its own arm, and it CATCHES (§3.4: an out-of-range
// endpoint raises IndexOutOfRangeException). The discharge is registered at
// the PRODUCTION, so §28.3's exception-path paragraph covers the already-
// produced, not-yet-consumed temporary during unwinding.
//
// ⚠ The `catch` is load-bearing, not cosmetic: the runtime's unhandled-
// exception exit path uses `_exit(70)`, which bypasses LSan's atexit handler,
// so an UNCAUGHT raise would report NO leak and this row would pass
// vacuously. Catching returns normally, so LSan runs.
//
// Pre-fix: 37 B in 2 allocations per iteration.
// =====================================================================
BEZEL_TEST(string_index_producer_receiver_discharge_7171,
           throw_edge_slice_over_producer_receiver_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(loop60(
        kProducer,
        "  try "
        "    var t: string := (k + \"cd\")[0..999]; "
        "    if t.Length > 0 then count := count + 1; end; "
        "  catch IndexOutOfRangeException => count := count + 1; end; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// =====================================================================
// (E) THE THROW EDGE, char-index spelling — the bounds check raises
// IndexOutOfRangeException (§4.6 / §31.2). Same catch requirement.
// =====================================================================
BEZEL_TEST(string_index_producer_receiver_discharge_7171,
           throw_edge_char_index_over_producer_receiver_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(loop60(
        kProducer,
        "  try "
        "    var c: char := (k + \"cd\")[999]; "
        "    if c = 'a' then count := count + 1; end; "
        "  catch IndexOutOfRangeException => count := count + 1; end; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// =====================================================================
// (F) CONTROL — the MEMBER-ACCESS form `(k + "cd").Length`. It was
// ALREADY clean before this fix (the `.Length` arm releases a +1 producer
// receiver), so it is the control that isolates the cause to the INDEX
// path. A change here is a REGRESSION — the classic symptom of a double
// discharge.
// =====================================================================
BEZEL_TEST(string_index_producer_receiver_discharge_7171,
           member_access_control_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(loop60(
        kProducer,
        "  var n := (k + \"cd\").Length; "
        "  if n = 4 then count := count + 1; end; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// =====================================================================
// (G) CONTROL — the BOUND-RECEIVER form: the producer is bound to a local
// first, and the INDEX reads the bound local. It was ALREADY clean (the
// local's own scope-end disposal covers it) and must stay clean: the new
// registration is gated on the receiver being an UNBOUND producer, so a
// bound receiver is never registered (and never double-released).
// =====================================================================
BEZEL_TEST(string_index_producer_receiver_discharge_7171,
           bound_receiver_control_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(loop60(
        kProducer,
        "  var s: string := k + \"cd\"; "
        "  var c: char := s[0]; "
        "  if c = 'a' then count := count + 1; end; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// =====================================================================
// (H) CONTROL — the SLICE over a BOUND receiver (`s[0..3]`), which was
// already clean and must stay clean: the new registration must not touch a
// bound receiver (no double discharge).
// =====================================================================
BEZEL_TEST(string_index_producer_receiver_discharge_7171,
           bound_receiver_slice_control_lsan_clean_spec_4_6) {
    JitResult r = runAotChecked(loop60(
        "var s: string := \"abcd\"; ",
        "  var t: string := s[0..3]; "
        "  if t.Length = 4 then count := count + 1; end; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// =====================================================================
// (I) The RESULT of the index is ITSELF a producer (a fresh `string`), and
// it is consumed by a further concat. Both the receiver's reference and the
// result's reference must be discharged exactly once — this row is the
// double-discharge guard for the interaction.
// =====================================================================
BEZEL_TEST(string_index_producer_receiver_discharge_7171,
           sliced_producer_consumed_by_concat_lsan_clean_spec_4_6) {
    JitResult r = runAotChecked(loop60(
        kProducer,
        "  var t: string := (k + \"cd\")[0..2] + \"XY\"; "
        "  if t.Length = 5 then count := count + 1; end; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// =====================================================================
// (J) TWO producer receivers in ONE scope — the second must not inherit the
// first one's discharge, and neither may be released twice.
// =====================================================================
BEZEL_TEST(string_index_producer_receiver_discharge_7171,
           two_producer_receivers_in_one_scope_lsan_clean_spec_28_3) {
    JitResult r = runAotChecked(loop60(
        kProducer,
        "  var a: string := (k + \"cd\")[0..2]; "
        "  var b: string := (k + \"ef\")[0..2]; "
        "  if a.Length + b.Length = 6 then count := count + 1; end; "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}
