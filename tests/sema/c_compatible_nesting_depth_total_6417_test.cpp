// fix #6417 (spec §32.13, v0.186) — the §32.13 C-compatible record walk is
// TOTAL; `isCCompatibleRecordSym` used to carry a magic `depth > 8` cap.
//
// THE DEFECT. §32.13 clause 1 computes C-compatibility recursively: "every
// field's type is an FFI-compatible scalar (…), an inline array [N]T of such a
// scalar (§32.5's element set), or another C-compatible record (nesting;
// recursion is impossible — records are value types)".  The clause states the
// nesting rule with NO depth qualifier, and it terminates on the *shape* of
// the graph (a record cannot contain itself by value), not on its size.
//
// `isCCompatibleRecordSym` nevertheless returned `false` outright once the
// nesting walk passed `depth > 8` — a bound on this compiler, silently
// reported as a verdict about the *program*.  A 10-level C-compatible record
// chain was therefore mis-classified NOT C-compatible, and §32.13's clause-3
// admission check (`checkExternType`) turned that false verdict into a
// diagnostic stating something factually wrong about the type:
//
//   "extern method parameter `r` has record type `R0`, which is not
//    C-compatible: every field's type must be an FFI-compatible scalar (…)"
//
// Every one of those fields IS an FFI-compatible scalar or another
// C-compatible record.  Both the BY-VALUE and the `out`/`ref` spellings were
// affected: the by-address shortcut at `checkExternType` calls the same
// `isCCompatibleRecord`, so a chain ≥ 10 levels deep was rejected there too.
//
// THE FIX (7eb7a550) removes the cap and replaces it with a per-path cycle
// guard (`std::unordered_set<const TypeSymbol*> onChain`, inserted on entry
// and erased on every exit).  The guard fires only on a symbol already on the
// current enclosing path — a genuine cycle — which a well-formed program cannot
// contain by value (§32.13 clause 1, and §9's own direct-recursion check).
//
// WHY EVERY ROW PINS DIAGNOSTIC TEXT, BOTH DIRECTIONS.
//
// A row asserting only "it did not run" pins NOTHING: the pre-fix compiler
// also rejected every program here, it just rejected them with a different —
// wrong — message.  Such a row is green on the broken compiler, so it is a
// vacuous oracle.  Each reject row below therefore asserts BOTH:
//   (1) the CORRECT diagnostic is present, and
//   (2) the WRONG one (`not C-compatible`) is ABSENT.
// (1) alone would also pass pre-fix (it rejected too, just wrongly); (2) is
// what breaks the vacuity, and (1) is what stops the row being satisfied by
// some unrelated rejection.  `failedAtCompileTimeWithRealLocation` adds a third
// leg: the diagnostic carries a real `<path>:<line>:<col>` prefix, so it came
// from the compiler reaching the shape and not from a crash or a timeout —
// without which a parse error could satisfy both text assertions.
//
// BEHAVIOUR FLIP (named, with its new expectation).  The only verdict this
// fix changes is the §32.13 classification of a record nested 9 levels or
// deeper (i.e. reached at `depth > 8`), when the chain is in fact
// C-compatible:
//   * by value at an extern parameter: was "not C-compatible" → NOW
//     "C-compatible but admitted by address only" (clause 3 is still the
//     reason it is rejected; only the stated *reason* is now true).
//   * `out`/`ref` at an extern parameter: was "not C-compatible" → NOW
//     ADMITTED (clause 3's by-address admission).
// No existing gtest or lit test pinned the old verdict, so no other test
// changes expectation.  The rows below carry the new ones.
//
// THE DEPTHS.  `chainOf(n)` builds n records, `R0` containing `R1` … `R(n-1)`
// containing `Int32`; the classification of `R(i)` is reached at `depth == i`.
// The old cap rejected at `depth > 8`, so `n == 9` (reaching `depth == 8`)
// was the last accepted chain and `n == 10` the first mis-classified one.
// Depth 9 is kept as the BELOW-boundary control that must behave identically
// before and after the fix, so a row that pins the boundary is not merely
// pinning "deep"; depths 20, 64 and 70 clear the old cap by a wide margin and
// cover the `int` budget a long chain consumes.
//
// THE REVISION CITED.  v0.186 is where §32.13 landed: the v0.186 changelog
// entry introduces "C-compatible records (§32.13; §3.6) … By-address
// admission (`ref`/`out`, `Ptr<R>`); single-integer/pointer-field records pass
// directly (typed handles)", i.e. clauses 1, 3 and 4 all together.  The text
// read to write these rows is the document at its CURRENT revision, v0.338
// (`docs/Bezel_Language_Spec.md` header); a `grep -c '32\.13'` over every
// changelog entry from v0.306 onward returns 0, so v0.306 — the revision the
// surrounding `src/sema` comments carry — is not a revision §32.13 was ever
// revised at, and is not cited here.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains,
                                            // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The CORRECT diagnostic for a C-compatible record written by value — §32.13
// clause 3 ("**By value is not admitted**, except the single-field form").
// Verbatim from the emission site in `checkExternType`.
const char* kByAddressOnly =
    "which is C-compatible but admitted by address only";

// The WRONG diagnostic the depth cap produced — the factually false claim
// that some field of a wholly C-compatible chain is neither a scalar, an
// inline array, nor a C-compatible record.  Its ABSENCE is what makes these
// rows non-vacuous: the pre-fix compiler emitted exactly this string.
const char* kNotCCompatible = "which is not C-compatible: every field's type";

// n nested records, `R0` containing `R1` … `R(n-1)` containing `Int32`.
// Each `Ri` is C-compatible under §32.13 clause 1 (its field is either an
// FFI-compatible scalar, or another C-compatible record), so the whole chain is.
std::string chainOf(int n) {
    std::string r = "module M; ";
    for (int i = n - 1; i >= 0; --i) {
        r += "public record R" + std::to_string(i) + "(F: ";
        r += (i == n - 1) ? "Int32); " : ("R" + std::to_string(i + 1) + "); ");
    }
    return r;
}

// The same chain of n records, but the innermost field is a `string` — a
// disqualifier under §32.13 clause 1 at EVERY depth.  Used to pin that the
// total walk did not become over-permissive (a silent-acceptance inversion:
// the walk now reaches depths it never reached before, so it must reach them
// and still say no).
std::string chainOfWithStringLeaf(int n) {
    std::string r = "module M; ";
    for (int i = n - 1; i >= 0; --i) {
        r += "public record R" + std::to_string(i) + "(F: ";
        r += (i == n - 1) ? "string); " : ("R" + std::to_string(i + 1) + "); ");
    }
    return r;
}

const char* kMain = "class C public static method Main(): int => 0; end C; ";

// A C-compatible chain of `n` levels taken BY VALUE at an extern parameter —
// rejected by §32.13 clause 3.
std::string byValue(int n) {
    return chainOf(n) + "extern \"libc\" method Take(r: R0): int; end; " + kMain;
}

// A C-compatible chain of `n` levels taken BY ADDRESS (`out`) — ADMITTED by
// §32.13 clause 3, and it must compile, link and run.
std::string byOut(int n) {
    return chainOf(n) + "extern \"libc\" method Take(out r: R0): int; end; " +
           kMain;
}

// Two records referring to each other THROUGH a `Ptr` head.  §32.13 clause 1
// disqualifies `Ptr<T>` as a *field* type for the C-struct layout, but the
// walk reaches the pointee first (via the FFI-scalar / FFI-pointee arms), so
// `R0 → Ptr<R1> → R1 → Ptr<R0>` re-enters `isCCompatibleRecordSym` on a symbol
// already on the path.  That is the only input the new cycle guard exists for,
// and it must terminate rather than recurse.
std::string cyclicThroughPtr() {
    return std::string(
               "module M; public record R0(F: Ptr<R1>); "
               "public record R1(F: Ptr<R0>); ") +
           "extern \"libc\" method Take(out r: R0): int; end; " + kMain;
}

}  // namespace

// ===========================================================================
// THE REGRESSION ROWS — by value.  Pre-fix: "which is not C-compatible: every
// field's type …".  Post-fix: "which is C-compatible but admitted by address
// only".  The second assertion is the one that fails pre-fix.
// ===========================================================================

// Depth 9 is the boundary CONTROL: the last depth the old cap reached, so
// this row must behave identically before and after the fix — it is what
// shows the rows below fail because of the cap and not because deep chains
// are rejected for some other reason.

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_9_by_value_reports_c_compatible_by_address) {
    auto r = runAot(byValue(9));
    BEZEL_EXPECT_FALSE(r.ran);
    // a real <path>:<line>:<col> prefix: the compiler REACHED the §32.13
    // check, so no parse error or crash can stand in for it
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // (1) the CORRECT diagnostic — clause 3 is the real reason it is refused
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kByAddressOnly));
    // (2) the WRONG one the depth cap produced must be ABSENT
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNotCCompatible));
}  // boundary control

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_10_by_value_reports_c_compatible_by_address) {
    auto r = runAot(byValue(10));
    BEZEL_EXPECT_FALSE(r.ran);
    // a real <path>:<line>:<col> prefix: the compiler REACHED the §32.13
    // check, so no parse error or crash can stand in for it
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // (1) the CORRECT diagnostic — clause 3 is the real reason it is refused
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kByAddressOnly));
    // (2) the WRONG one the depth cap produced must be ABSENT
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNotCCompatible));
}  // first depth the cap mis-classified

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_16_by_value_reports_c_compatible_by_address) {
    auto r = runAot(byValue(16));
    BEZEL_EXPECT_FALSE(r.ran);
    // a real <path>:<line>:<col> prefix: the compiler REACHED the §32.13
    // check, so no parse error or crash can stand in for it
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // (1) the CORRECT diagnostic — clause 3 is the real reason it is refused
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kByAddressOnly));
    // (2) the WRONG one the depth cap produced must be ABSENT
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNotCCompatible));
}

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_20_by_value_reports_c_compatible_by_address) {
    auto r = runAot(byValue(20));
    BEZEL_EXPECT_FALSE(r.ran);
    // a real <path>:<line>:<col> prefix: the compiler REACHED the §32.13
    // check, so no parse error or crash can stand in for it
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // (1) the CORRECT diagnostic — clause 3 is the real reason it is refused
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kByAddressOnly));
    // (2) the WRONG one the depth cap produced must be ABSENT
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNotCCompatible));
}

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_64_by_value_reports_c_compatible_by_address) {
    auto r = runAot(byValue(64));
    BEZEL_EXPECT_FALSE(r.ran);
    // a real <path>:<line>:<col> prefix: the compiler REACHED the §32.13
    // check, so no parse error or crash can stand in for it
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // (1) the CORRECT diagnostic — clause 3 is the real reason it is refused
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kByAddressOnly));
    // (2) the WRONG one the depth cap produced must be ABSENT
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNotCCompatible));
}  // past the sibling `baseTypePathTo` budget of 64

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_70_by_value_reports_c_compatible_by_address) {
    auto r = runAot(byValue(70));
    BEZEL_EXPECT_FALSE(r.ran);
    // a real <path>:<line>:<col> prefix: the compiler REACHED the §32.13
    // check, so no parse error or crash can stand in for it
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // (1) the CORRECT diagnostic — clause 3 is the real reason it is refused
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kByAddressOnly));
    // (2) the WRONG one the depth cap produced must be ABSENT
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNotCCompatible));
}


// ===========================================================================
// THE REGRESSION ROWS — by address.  §32.13 clause 3 admits a C-compatible
// record "as `ref`/`out` extern parameters".  Pre-fix these did NOT run: the
// `out` shortcut calls the same capped `isCCompatibleRecord`, so at depth >= 10
// it fell through to the clause-3 rejection and the program was refused with
// "not C-compatible".  These rows therefore assert the stronger property —
// the program compiles, links and exits 0 — and need no diagnostic needle:
// `r.ran` is itself the verdict.
// ===========================================================================


BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_9_by_out_admitted_and_runs) {
    auto r = runAot(byOut(9));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}  // boundary control

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_10_by_out_admitted_and_runs) {
    auto r = runAot(byOut(10));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}  // first depth the cap refused outright

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_16_by_out_admitted_and_runs) {
    auto r = runAot(byOut(16));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_20_by_out_admitted_and_runs) {
    auto r = runAot(byOut(20));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_64_by_out_admitted_and_runs) {
    auto r = runAot(byOut(64));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}  // past the sibling `baseTypePathTo` budget of 64

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_70_by_out_admitted_and_runs) {
    auto r = runAot(byOut(70));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// THE OVER-PERMISSIVENESS GUARD.  A walk that is now TOTAL reaches field types
// it never reached before; it must reach them and still disqualify them.  A
// `string` leaf is disqualifying under §32.13 clause 1 at every depth, so both
// rows assert the SPECIFIC `not C-compatible` wording.  A pre-fix compiler
// happens to reach the same verdict here (the cap returned false for the wrong
// reason, but by the same door) — these rows are guards, not the regression
// pin, and they are labelled as such: what they rule out is the total walk
// admitting a chain it never inspected.
// ===========================================================================

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_1_string_leaf_reports_not_c_compatible) {
    auto r = runAot(chainOfWithStringLeaf(1) +
                    "extern \"libc\" method Take(r: R0): int; end; " + kMain);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNotCCompatible));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kByAddressOnly));
}

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           depth_20_string_leaf_reports_not_c_compatible) {
    auto r = runAot(chainOfWithStringLeaf(20) +
                    "extern \"libc\" method Take(r: R0): int; end; " + kMain);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNotCCompatible));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kByAddressOnly));
}

// ===========================================================================
// THE CYCLE GUARD — the instrument that replaced the cap must actually
// terminate.  The verdict is the same pre-fix and post-fix (both say "not
// C-compatible", for different reasons), so this row pins TERMINATION, not
// classification: it asserts the specific diagnostic, which a hang, a stack
// overflow or a crash could not produce.
// ===========================================================================

BEZEL_TEST(c_compatible_nesting_depth_total_6417,
           cyclic_ptr_chain_terminates_with_not_c_compatible) {
    auto r = runAot(cyclicThroughPtr());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNotCCompatible));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "did not finish within"));
}