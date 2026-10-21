// Codegen / runtime — §8.5 / §9.1 / §9.2 / §4.6 / §28.9 (#2065): a string
// returned by a SYNTHESIZED record `ToString()` call and assigned to a plain
// `var` local (`var s := r.ToString();`) must have its single +1 producer
// reference decremented at the local's scope-end release.
//
// §8.5 / §9.1 / §9.2 (v0.204): a positional or auto-hashable body
// record's ToString is the synthesized `"TypeName(Field: value, …)"` form —
// a method-call RESULT.
//
// §4.6 / §28.9 (v0.204): a `+1 string PRODUCER` initializer transfers its
// single reference into the local's slot (no retain), and a local proven
// non-escaping by the conservative escape walk is released at scope end —
// dropping the single reference exactly once (rc 1→0). No double-free: no
// extra retain is emitted.
//
// THE BUG (#2065). `Emitter::isPlusOneStringProducer` recognized the
// primitive-ToString, StringBuilder.ToString, Guid/DateTime/TimeSpan/DateOnly/
// TimeOnly ToString, and string.Join forms — but NOT the synthesized record
// ToString. So `var s := r.ToString();` was classified as a BORROW: the local
// was retained at bind (the producer's +1 became rc 2) and released at scope
// end (rc 2→1), so the producer's original +1 was NEVER decremented → leak.
// Each iteration of a loop leaked the 32-byte `bz.string` header + its data
// buffer. Discovered during the #2060 (boxed-Object leak) variant design — the
// #2060 lane omitted a ToString test variant specifically to keep that bug
// #2060-specific; this is the method-call-return-into-local shape, promoted to
// its own issue as #2065 (distinct from #1994's WriteLine-arg leak and #2023's
// ctor-arg leak).
//
// THE FIX. `isPlusOneStringProducer` now has an arm that recognizes a no-arg
// `ToString()` on a Named RECORD receiver (positional OR auto-hashable
// body — exactly the shapes lowerValueSemMethodOn routes to emitRecordToString,
// which mints a fresh rc=1 heap string per §9.1). With the recognition, the
// local is classified as a +1 producer: the bind TRANSFERS the single
// reference (no retain), the escape walk proves the local non-escaping (the
// only use is `s.Length` — a safe-read member access), and the scope-end
// release drops rc 1→0 — freed exactly once. The ineligible-body-record and
// enum cases (whose ToString returns a pooled literal sentinel, NOT +1 per
// §8.5) are excluded by the autoBearing gate, so the recognition does not fire
// there.
//
// OBSERVABILITY. The leak is a DIRECT leak (no other reference reaches the
// returned string), so runAotChecked's LSan flags it: WITHOUT the fix `r.ran`
// is false (Direct+Indirect leak of the bz.string header + data buffer per
// iteration); WITH the fix the run is clean AND no double-free / UAF is
// introduced (the dispose-exactly-once net rides on `r.ran`).
//
// TEST SHAPE. A ZERO-FIELD positional record (`record Rec();`) is used so the
// synthesized ToString yields exactly ONE heap allocation (`"Rec()"` — a single
// emitStringConcat of the literal `"Rec("` with the literal `")"`) per
// iteration. This isolates the #2065 mechanism — the scope-end release of the
// returned string — from a SEPARATE pre-existing leak path in
// emitRecordToString's intermediate-concat chain (each intermediate is a +1
// producer that is borrowed-then-dropped; a multi-field record would conflate
// the two leak paths and obscure the #2065 anchor).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// §8.5 / §9.1 / §4.6 / §28.9 / #2065: the issue's exact pattern. A zero-field
// positional record `r`, then `r.ToString()` is assigned to the inferred local
// `s`. The synthesized ToString allocates a fresh rc=1 `Rec()` string; WITHOUT
// the fix the producer's +1 leaks each iteration (198 allocations over 99 iters
// — the bz.string header + its data buffer, both directly leaked). 100
// iterations amplify the leak so LSan catches it decisively. The exit code is
// `sink mod 256` — the program's own observable value (independent of the
// leak). WITHOUT the fix LSan flips r.ran false; WITH it the run is clean.
const char* kRecordToStringLocalLoop =
    "module M; "
    "record Rec(); "                                                       // zero-field positional
    "class C public static method Main(): int "
    "  var j := 0; var sink := 0; "
    "  while j < 100 do "
    "    var r := new Rec(); "
    "    var s := r.ToString(); "          // synthesized ToString → fresh rc=1
    "    sink := sink + s.Length; "        // safe-read (member access on s)
    "    j := j + 1; "
    "  end; "
    "  return sink mod 256; "
    "end Main; end C;";

}  // namespace

// =====================================================================
// #2065 — `var s := r.ToString();` on a positional record releases the
// returned +1 string at scope end (§8.5 / §9.1 / §4.6 / §28.9); no leak.
// The cardinal repro from the issue (loop-amplified for LSan decisiveness).
// =====================================================================

BEZEL_TEST(codegen_record_tostring_local_leak_2065,
           record_tostring_local_does_not_leak_spec_8_5_9_1_28_9) {
    // spec §8.5 / §9.1 / §4.6 / §28.9, v0.204: a synthesized record ToString
    // returns a fresh rc=1 heap string; the inferred local that owns the single
    // producer reference must release it at scope end. WITHOUT the fix the
    // escape walk excluded the local (treated as a borrow) so the +1 was never
    // decremented — every iteration leaked the bz.string header + data buffer
    // (Direct+Indirect under LSan) — LSan flips r.ran false. WITH the fix the
    // local is a +1 producer (transfer at bind, no retain) and the scope-end
    // release drops rc 1→0 (no leak, no double-free).
    JitResult r = runAotChecked(kRecordToStringLocalLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));

    BEZEL_EXPECT_TRUE(r.ran);   // LSan-clean (the #2065 leak would flip this)
}
