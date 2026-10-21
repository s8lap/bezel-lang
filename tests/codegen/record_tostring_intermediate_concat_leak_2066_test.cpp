// Codegen / runtime — §9.1 / §9.2 / §4.6 / §28.9 (#2066): the SYNTHESIZED
// record ToString's INTERMEDIATE concat buffers must be released mid-chain.
//
// §9.1 / §9.2 / §8.5 (v0.204): a positional or auto-hashable body
// record's ToString yields the synthesized form `"TypeName(Field: value, …)"`.
// `Emitter::emitRecordToString` (src/codegen/LlvmBackend_String.cpp) builds it
// by chaining `emitStringConcat` calls: an accumulator starts at the literal
// `"TypeName("` and is repeatedly concatenated with the per-field prefix
// (`"Field: "`) and the field's own ToString, separated by `", "`, then closed
// with `")"`.
//
// §4.6 / §28.9 (v0.204): each `emitStringConcat` mints a fresh rc=1 `bz_string`
// whose DATA buffer is a byte-copy of its operands; the operands themselves
// are NOT retained by the result. A +1 string producer consumed only by such a
// concat must therefore be released after the concat, mirroring BinaryExpr
// `+`'s `releaseConcatIntermediate` (src/codegen/LlvmBackend_Exprs.cpp).
//
// THE BUG (#2066). Without a mid-chain release, every intermediate accumulator
// (each previous `emitStringConcat` result, a fresh +1 producer) and every
// value-typed field's ToString result (int via `bz_int_to_string`, etc., also
// a fresh +1) leaks: nothing decrements their rc. The FINAL concat — the
// string returned to the caller — is released at the caller's scope end (that
// is the #2065 fix), but every intermediate leading up to it leaks. A
// multi-field record therefore leaks multiple bz.string headers + data
// buffers per ToString call. The #2065 test deliberately used a ZERO-field
// record (exactly one concat, no intermediates) to isolate the scope-end
// mechanism; this test uses a MULTI-field record to exercise the intermediates.
//
// THE FIX. `emitRecordToString` now tracks whether the running accumulator is
// a fresh +1 producer (any `emitStringConcat` result) or the initial literal
// (a sentinel — release is a no-op), and after each concat that reassigns the
// accumulator it releases the PREVIOUS accumulator if it was a +1 producer.
// It also releases the per-field ToString result when it is a +1 producer,
// EXCEPT for a STRING field whose ToString is identity (§9.6 —
// `emitPrimitiveToString` returns `val` itself), which is a BORROW of the
// record's own slot and must not be released (the record still owns it;
// releasing would double-free on the record's own disposal). A literal-form
// field ToString (bool.ToString → "True"/"False" sentinel; a class without
// ToString → type-name sentinel) has sentinel rc, so its release is a no-op.
// The FINAL accumulator is returned to the caller (the +1 it binds) and is
// NOT released here.
//
// OBSERVABILITY. The leaks are DIRECT (no other reference reaches the
// intermediate buffers), so runAotChecked's LSan flags them: WITHOUT the fix
// `r.ran` is false (Direct+Indirect leak summary across thousands of bz.string
// headers + data buffers — the exact count depends on field count); WITH the
// fix the run is clean AND no double-free / UAF is introduced (the
// dispose-exactly-once net rides on `r.ran`).
//
// TEST SHAPE. A THREE-FIELD positional record `Rec(A: int, B: int, C: int)`
// in a 100-iteration loop amplifies the leak for LSan decisiveness (each
// iteration's ToString chains 7 concats and 3 int ToString producers). The
// exit code `sink mod 256` is the program's own observable value, independent
// of the leak. WITHOUT the fix LSan flips r.ran false; WITH the fix r.ran is
// true and the binary exits cleanly with its own return value.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// §9.1 / §4.6 / §28.9 / #2066: the issue's pattern, multi-field. A 3-field
// positional record `r`, then `r.ToString()` is assigned to the inferred
// local `s`. The synthesized ToString builds `"Rec(A: …, B: …, C: …)"` via a
// chain of 7 emitStringConcat calls + 3 per-field int ToString producers; the
// FINAL string is released at scope end (the #2065 fix), but WITHOUT this
// fix every INTERMEDIATE concat buffer + every per-field int ToString leaks
// (Direct+Indirect under LSan, ~22 allocations × 100 iterations). 100
// iterations amplify the leak so LSan catches it decisively. The exit code
// `sink mod 256` is the program's own observable value (independent of the
// leak). WITHOUT the fix LSan flips r.ran false; WITH the fix the run is
// clean AND no double-free / UAF is introduced.
const char* kRecordToStringMultiFieldLoop =
    "module M; "
    "record Rec(A: int, B: int, C: int); "                                // 3-field
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 100 do "
    "    var r := new Rec(j, j + 1, j + 2); "
    "    var s := r.ToString(); "          // 7 intermediate concats + 3 int ToString
    "    sink := sink + s.Length; "        // safe-read (member access on s)
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

// §9.1 / §4.6 / §28.9 / #2066: a record with a STRING field — confirms the
// fix does NOT release the borrowed string field's identity ToString. WITHOUT
// the borrow guard, releasing a string field's ToString result would
// decrement the rc of the record's own slot — a double-free when the record
// is later disposed. WITH the borrow guard the string field is left alone.
// runAotChecked's LSan / ASan catches a double-free as a heap-use-after-free
// (the record's disposal dereferences the freed slot) → r.ran false.
const char* kRecordToStringStringFieldLoop =
    "module M; "
    "record Rec(A: int, Name: string); "                                  // string field
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 100 do "
    "    var r := new Rec(j, \"x\"); "
    "    var s := r.ToString(); "          // int field ToString is +1; string is BORROW
    "    sink := sink + s.Length; "
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

}  // namespace

// =====================================================================
// #2066 — emitRecordToString releases intermediate concat buffers and
// per-field ToString +1 producers (§9.1 / §4.6 / §28.9); multi-field.
// =====================================================================

BEZEL_TEST(codegen_record_tostring_intermediate_concat_leak_2066,
           record_tostring_intermediate_concat_does_not_leak_spec_9_1_4_6_28_9) {
    // spec §9.1 / §4.6 / §28.9, v0.204: a multi-field synthesized record
    // ToString chains emitStringConcat calls; each intermediate accumulator
    // (a +1 producer) and each value-typed field's ToString result (a +1
    // producer) must be released after its concat consumes it, mirroring
    // BinaryExpr `+`'s releaseConcatIntermediate. WITHOUT the fix every
    // intermediate leaks (Direct+Indirect under LSan) — LSan flips r.ran
    // false. WITH the fix the intermediates are released mid-chain (no leak,
    // no double-free).
    JitResult r = runAotChecked(kRecordToStringMultiFieldLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));

    BEZEL_EXPECT_TRUE(r.ran);   // LSan-clean (the #2066 leak would flip this)
}

BEZEL_TEST(codegen_record_tostring_string_field_no_double_free_2066,
           record_tostring_string_field_borrow_not_released_spec_9_1_9_6) {
    // spec §9.1 / §9.6 / §4.6 / §28.9, v0.204: a STRING field's ToString is
    // identity (§9.6) — a BORROW of the record's own slot, not a +1 — so it
    // must NOT be released at the concat's consume point (releasing would
    // decrement the record's own field rc, and the record's later disposal
    // would double-free). runAotChecked's ASan catches the double-free as a
    // heap-use-after-free → r.ran false. WITH the borrow guard the string
    // field is left alone; only the int field's +1 ToString is released.
    JitResult r = runAotChecked(kRecordToStringStringFieldLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));

    BEZEL_EXPECT_TRUE(r.ran);   // no double-free / no leak
}
