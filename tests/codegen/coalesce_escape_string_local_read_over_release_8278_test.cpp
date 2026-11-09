// Codegen / refcount — a `??` READ of an ESCAPING producer-initialized
// `string?` local must not hand the slot's single +1 to its consumer, or the
// SECOND read over-releases it (rc 134, §28.9). Issue #8278.
//
// Spec: §28.9 (runtime-managed `string` — the runtime manages the value's
// lifetime), §28.3 (the ownership kernel: "an unmarked read of a named binding
// lends" — K3.13, no silent consume), §28.11 (the element slot policy: a
// borrowing collection's runtime-managed-reference slot holds a SHARED
// reference, so the holder continues to own the element). Spec revision v0.355.
//
// THE BUG. `var v: string? := "b" + n.ToString();` binds a +1 producer string
// local. `v ?? "none"` READS it — twice. Codegen treated the read as a
// TRANSFER-OUT of the slot's single reference (the `returnedStringLocalOwnsPlusOne`
// guard in `lowerCoalesce` skipped the arm's retain whenever the operand was a
// local that owns a +1 and escapes), so the consumer's release dropped the
// SLOT's reference on the first read and the second read released a reference
// the slot no longer had: "bezel: fatal: refcount over-release (string) — two
// releases for one reference; terminating per §28.9" (rc 134). The same shape
// with the local captured by a lambda was worse — a use-after-free (ASan
// SEGV in `bz_string_to_utf8z` on the second read), because the closure
// BORROWS the slot's reference and the first read freed it.
//
// THE FIX (two coordinated halves, the #1951 pair). (1) `lowerCoalesce`: a
// `??` arm that reads a string local MINTs its own +1 (the retain the arm
// already had for a non-producer operand) — a read is a borrow-read, not a
// transfer-out, and it may happen N times. (2) StringEscapeWalker: a store
// into a BORROWING built-in `Dictionary<K,string>` / `Dictionary<K,string?>`
// indexer (`d[k] := v;`) RETAINS the value into the table (§28.11's shared
// reference), so the source local keeps its OWN +1 and its scope-end release
// must fire. The two halves move in lockstep: the read no longer consumes the
// slot's reference, and the slot's reference is discharged exactly once (the
// scope-end release) instead of never.
//
// WHY THE SECOND HALF IS REQUIRED. Without it the escaping local's +1 would
// have no discharge point at all — the scope-end release stays unarmed for an
// escaping local — so the fix would trade the over-release for a LEAK (the
// measured intermediate state: the issue's own repro printed "b3"/"b3" and
// then leaked the string's 32-byte header + 3-byte payload under LSan). The
// pair is one fix, not two.
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan (the checked
// oracle, per the lane contract's disposal/ownership rule). The leak rows
// LOOP 50x so a leak is 50 objects, not one — a single leak can hide under
// the sanitizer's noise. Every row pins the program's stdout as well as its
// exit status, so a premature free (double-free/UAF) corrupts the value and
// trips ASan rather than passing quietly.
//
// No toolchain-SKIP guard here: a box without the C toolchain must FAIL these
// rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP burn-down —
// the checker's remedy is to delete the guard, not to return on it).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// (A) The issue's repro. The local `v` is a +1 producer (a concatenation) and
// it ESCAPES: `d[1] := v;` stores it into a built-in Dictionary, whose slot
// holds a shared reference (§28.11) — the dictionary takes its own +1 and the
// local keeps its own. Two `??` reads of `v`.
//
// Pre-fix: the first read handed the slot's single +1 to the consumer, whose
// release dropped it; the second read released a reference the slot no longer
// had → "refcount over-release (string) — two releases for one reference;
// terminating per §28.9" (exit 134, no output past the first line).
// Post-fix: both reads mint their own +1, the local's +1 is discharged by its
// scope-end release, and both reads print the value.
const char* kEscapingTwoReads =
    "module M; import System; "
    "class C "
    "  public static method Main(): int "
    "    var n := 3; "
    "    var v: string? := \"b\" + n.ToString(); "
    "    var d := new Dictionary<int, string?>(); "
    "    d[1] := v; "
    "    Console.WriteLine(v ?? \"none\"); "
    "    Console.WriteLine(v ?? \"none\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (B) The same escaping shape, ONE read, LOOPED 50x. This is the balance
// row: the read must not consume the slot's +1 AND the slot's +1 must still
// be discharged exactly once (the scope-end release the walker relaxation
// re-arms for the dict store). One read per iteration is enough to expose an
// over-release, and 50 iterations make a leak 50 objects.
//
// Pre-fix: clean (the one read consumed the slot's reference, so the
// dictionary's release balanced it) — this row is the CONTROL for the
// over-release and the PIN for the leak: with only the coalesce half of the
// fix it leaks 50 x (32-byte header + 3-byte payload).
const char* kEscapingOneReadLoop =
    "module M; import System; "
    "class C "
    "  public static method Main(): int "
    "    var i := 0; "
    "    while i < 50 do "
    "      var n := 3; "
    "      var v: string? := \"b\" + n.ToString(); "
    "      var d := new Dictionary<int, string?>(); "
    "      d[1] := v; "
    "      Console.WriteLine(v ?? \"none\"); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// (C) The escaping local stored into the dictionary and NEVER READ, LOOPED
// 50x. The store alone must leave the local's +1 discharged exactly once —
// the scope-end release the walker's dict-indexer relaxation re-arms. This row
// is independent of the coalesce (there is no `??` here at all): it pins the
// second half of the fix, which is the escaping producer local's own leak.
//
// Pre-fix: the store was a conservative escape, so the local lost its
// scope-end release and its +1 was orphaned — 50 leaked strings (LSan: 50 x
// 32-byte Direct leak + 50 x 3-byte Indirect leak).
const char* kEscapingStoredOnlyLoop =
    "module M; import System; "
    "class C "
    "  public static method Main(): int "
    "    var i := 0; "
    "    while i < 50 do "
    "      var n := 3; "
    "      var v: string? := \"b\" + n.ToString(); "
    "      var d := new Dictionary<int, string?>(); "
    "      d[1] := v; "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// (D) The NON-escaping control: the same two reads of the same producer
// local, with no dictionary store. The local's +1 lives in its slot and is
// discharged by the scope-end release, so the reads must borrow it (each
// minting its own +1) and the program must stay LSan-clean.
//
// This row is green on the BASE build and on the fixed build: it is the
// positive control that the fix does not change a shape that was already
// correct.
const char* kNonEscapingTwoReadsLoop =
    "module M; import System; "
    "class C "
    "  public static method Main(): int "
    "    var i := 0; "
    "    while i < 50 do "
    "      var n := 3; "
    "      var v: string? := \"b\" + n.ToString(); "
    "      Console.WriteLine(v ?? \"none\"); "
    "      Console.WriteLine(v ?? \"none\"); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// (E) The RHS-arm twin: `x ?? v` with a NULL left operand, so the RIGHT arm
// (the escaping local) is the one taken. The RHS arm had the same
// transfer-out guard, so the RHS read over-released the same way.
//
// The left operand is a `string?` local and the right one a non-nullable
// `string` producer local, so the `??` result is non-nullable and printable.
const char* kEscapingRhsArmTwoReads =
    "module M; import System; "
    "class C "
    "  public static method Main(): int "
    "    var n := 3; "
    "    var v: string := \"b\" + n.ToString(); "
    "    var d := new Dictionary<int, string?>(); "
    "    d[1] := v; "
    "    var x: string? := null; "
    "    Console.WriteLine(x ?? v); "
    "    Console.WriteLine(x ?? v); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (F) The LAMBDA twin: the local escapes through a CAPTURE (the closure
// borrows the slot's reference) instead of through a dictionary store, and is
// read twice through `??`. The closure keeps the slot's single +1 alive, so
// the pre-fix over-release freed a value the closure still held — the second
// read was a use-after-free (ASan SEGV in `bz_string_to_utf8z`).
//
// `Take` returns the local, so the slot's +1 leaves the method by the RETURN
// (the transfer-out path `returnedStringLocalOwnsPlusOne` describes) and the
// caller discharges it — which keeps the row LSan-clean instead of banking a
// second, unrelated residual (the closure capture's own borrow story, which
// is out of scope for this issue).
const char* kLambdaCapturedTwoReads =
    "module M; import System; "
    "class C "
    "  public static method Take(): string? "
    "    var n := 3; "
    "    var v: string? := \"b\" + n.ToString(); "
    "    var f := lambda() => v; "
    "    Console.WriteLine(v ?? \"none\"); "
    "    Console.WriteLine(v ?? \"none\"); "
    "    return v; "
    "  end Take; "
    "  public static method Main(): int "
    "    var r := C.Take(); "
    "    Console.WriteLine(r ?? \"none\"); "
    "    return 0; "
    "  end Main; "
    "end C;";

// The expected stdout of a looped row: `reps` copies of `line`.
std::string repeated(const char* line, int reps) {
    std::string out;
    for (int i = 0; i < reps; ++i) out += line;
    return out;
}

}  // namespace

// =====================================================================
// (A) The issue's repro — two reads of an escaping producer local. The
// second read must not over-release the slot's single +1 (§28.9).
// =====================================================================
BEZEL_TEST(coalesce_escape_string_local_read_over_release_8278,
           escaping_producer_local_two_reads_no_over_release_spec_28_9) {
    JitResult r = runAotChecked(kEscapingTwoReads);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);       // pre-fix: 134 (over-release fatal)
    BEZEL_EXPECT_EQ(r.stdoutText, "b3\nb3\n");
}

// =====================================================================
// (B) The balance row — one read of an escaping producer local, 50x. The
// read must not consume the slot's +1 and the slot's +1 must still be
// discharged exactly once (no leak).
// =====================================================================
BEZEL_TEST(coalesce_escape_string_local_read_over_release_8278,
           escaping_producer_local_one_read_loop_lsan_clean_spec_28_9) {
    JitResult r = runAotChecked(kEscapingOneReadLoop);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, repeated("b3\n", 50));
}

// =====================================================================
// (C) The escaping local stored into the dictionary and never read, 50x —
// the slot's +1 must be discharged by its scope-end release (no leak).
// =====================================================================
BEZEL_TEST(coalesce_escape_string_local_read_over_release_8278,
           escaping_producer_local_stored_only_loop_lsan_clean_spec_28_9) {
    JitResult r = runAotChecked(kEscapingStoredOnlyLoop);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
}

// =====================================================================
// (D) The non-escaping control — two reads of a producer local with no
// store: the reads borrow the slot's +1 and the scope-end release drops it
// (no leak, no over-release). Green on the base build too.
// =====================================================================
BEZEL_TEST(coalesce_escape_string_local_read_over_release_8278,
           non_escaping_producer_local_two_reads_loop_lsan_clean_spec_28_9) {
    JitResult r = runAotChecked(kNonEscapingTwoReadsLoop);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, repeated("b3\nb3\n", 50));
}

// =====================================================================
// (E) The RHS-arm twin — `x ?? v` over a NULL left operand, so the right
// arm (the escaping local) is taken twice.
// =====================================================================
BEZEL_TEST(coalesce_escape_string_local_read_over_release_8278,
           escaping_producer_local_rhs_arm_two_reads_no_over_release_spec_28_9) {
    JitResult r = runAotChecked(kEscapingRhsArmTwoReads);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);       // pre-fix: 134 (over-release fatal)
    BEZEL_EXPECT_EQ(r.stdoutText, "b3\nb3\n");
}

// =====================================================================
// (F) The lambda twin — the local escapes through a capture; the second
// read must not free the value the closure borrows (pre-fix: ASan SEGV).
// =====================================================================
BEZEL_TEST(coalesce_escape_string_local_read_over_release_8278,
           lambda_captured_producer_local_two_reads_no_use_after_free_spec_28_9) {
    JitResult r = runAotChecked(kLambdaCapturedTwoReads);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);       // pre-fix: 1 (ASan SEGV)
    BEZEL_EXPECT_EQ(r.stdoutText, "b3\nb3\nb3\n");
}
