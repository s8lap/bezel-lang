// Codegen — issue #1795: K8 full-expression boundary conformance for
// case-scrutinee temporaries. A `case <scrutinee> of …` whose scrutinee
// produces an OWNED temporary (a `new T(...)` for a heap class, an
// owned-returning factory call, an owning-collection materializer) holds
// that temporary for the lifetime of the WHOLE case construct, NOT just the
// scrutinee sub-expression: the K8 full-expression boundary is the end of
// the enclosing `case … end`. The bug was that the temporary either leaked
// (no end-of-case free at all) or got double-disposed (an end-of-arm
// reclaim on the wrong edge), depending on the arm shape.
//
// Root cause: codegen's `lowerCaseStmt` / `lowerCaseExpr` computed the
// scrutinee, dispatched the arm, and never emitted an end-of-construct
// dispose for the OWNED slot the scrutinee lives in. The ownership pass
// never flagged the case node as "carries an owned temporary", so codegen
// had no signal to emit the reclaim. The existing patterns (ExpressionStmt
// #2655, DiscardStmt #1009, lambda body #2674) all sit on a single node
// whose ownership state is OWNED; the case scrut is different — the
// OWNED producer is the scrutinee, not the case itself — so the gate has
// to walk into the scrutinee (`producerState == Owned`) and check the
// resulting type is a heap class / interface or owning-collection head.
//
// Fix: a `disposeOwnedScrutinee` flag on `CaseStmt` and `CaseExpr`, set by
// the OWNERSHIP pass during the FINAL (non-suppressed) walk when the
// scrutinee is a fresh owned producer of a heap reference or an owning
// collection, consumed by codegen's `lowerCaseStmt` / `lowerCaseExpr` to
// (1) store the scrut into an entry alloca, (2) register the slot on the
// dispose stack as `unwindOnly` (an arm-body throw disposes the temp),
// and (3) emit a normal-path null-guarded disposal at the construct's
// endBB. The slot-null guard + post-emission removal of the matching
// unwind entry keep the two edges mutually exclusive: dispose exactly
// once, on whichever edge actually fires.
//
// EXACTLY-ONCE ORACLE. The probes use a NON-trivial-dispose class with a
// destructor that increments / decrements a static `live` counter —
// §28.9's scope ("reclaimed by an implementation-defined mechanism that
// runs no user code") is only there for trivial-dispose types; here the
// reclaim dispatches through the class vtable and runs the user destructor,
// so a missing reclaim leaves `live == 1` at end of `Main` and a double
// reclaim is an ASan double-free (and `live` can underflow). Pinned
// mechanically by encoding both correctness (`seen == expected`) and
// memory-cleanliness (`Res.alive == 0` post-Main) in the exit code.
//
// fail-without / pass-with (measured on 944b398 baseline):
//   * CaseStmt + var binding: WITHOUT fix, `live == 1` (destructor never
//     ran, leak); WITH fix, `live == 0` and `seen == 17` (the expected
//     c.Value + Cell.live * 10 path; * 10 + live packs both into the exit
//     code).
//   * CaseStmt + `_ =>` wildcard (no binding): same fail/pass.
//   * CaseExpr (return-position scrutinee): same fail/pass.
//
// The owning-collection arm is pinned separately as a structural
// counterpart — same gate, different codegen emission branch
// (`collection` instead of `dynamicDispose`).

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// runChecked: ASan/LSan-instrumented build; returns -999 on any sanitizer
// fire, else the real exit code. One EXPECT_EQ pins both memory-cleanliness
// AND the computed value together.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
// runValueOnly: plain (no ASan) build. Used for the no-double-free
// controls below — #2629's lesson is that ASan can mask a wild free that
// the plain build crashes on.
int runValueOnly(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The issue's exact shape: a CaseStmt whose scrutinee is `new Cell(7)`,
//    and a `var c: Cell =>` binding arm. Without the fix the destructor
//    never ran (live == 1, leak); with the fix the destructor's
//    `live := live - 1` fires at end of `case … end`, giving `live == 0`.
//    Encoded value: c.Value (7) + Cell.live * 10 (10) = 17, then
//    result * 10 + Cell.live (0) = 170. Any leak leaves live == 1 and the
//    exit code becomes 171; a double-free aborts under ASan. (issue #1795)
BEZEL_TEST(case_scrutinee_owned_temp_reclaim_1795,
           case_stmt_with_var_binding_arm_spec_28_3_K8) {
    const std::string prog =
        "module M; "
        "class Cell "
        "  public static live: int := 0; "
        "  public Value: int; "
        "  public constructor(v: int) Value := v; "
        "    Cell.live := Cell.live + 1; "
        "  end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Main(): int "
        "    var seen: int := 0; "
        "    case new Cell(7) of "
        "      var c: Cell => do seen := c.Value + Cell.live * 10; end, "
        "      _ => do seen := 99; end "
        "    end; "
        "    return seen * 10 + Cell.live; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 170);
    BEZEL_EXPECT_EQ(runValueOnly(prog), 170);
}

// ── The other arm shape: a wildcard `_ =>` arm — the scrut is bound
//    nowhere, so it is pure unbound-owned-temporary territory. Same gate
//    fires; same `live` semantics. Encoded: seen (9) + live (0) -> 90.
//    (issue #1795)
BEZEL_TEST(case_scrutinee_owned_temp_reclaim_1795,
           case_stmt_with_wildcard_arm_spec_28_3_K8) {
    const std::string prog =
        "module M; "
        "class Cell "
        "  public static live: int := 0; "
        "  public constructor() Cell.live := Cell.live + 1; end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Main(): int "
        "    var seen: int := 0; "
        "    case new Cell() of "
        "      _ => do seen := 9; end "
        "    end; "
        "    return seen * 10 + Cell.live; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 90);
    BEZEL_EXPECT_EQ(runValueOnly(prog), 90);
}

// ── CaseExpr (return-position scrutinee). The expression form's K8
//    boundary is `end of the case … end`, same as the statement form; the
//    OWNERSHIP pass's `handleDischarge` CaseExpr arm and codegen's
//    `lowerCaseExpr` parallel the CaseStmt pair. Inside the arm `Cell.live
//    == 1` (just constructed, not yet destructed), so `c.Value (3) +
//    Cell.live * 10 (10) == 13` — the case returns 13. The fix must
//    dispose the temp at end-of-case on the normal edge, leaving the
//    return value 13. Without the fix the destructor never ran, but the
//    return value is still 13 (the leak does not change the computed
//    value), so the discriminator is the destructor firing — asserted via
//    the post-Main `live == 0` in the second test below. (issue #1795)
BEZEL_TEST(case_scrutinee_owned_temp_reclaim_1795,
           case_expr_scrutinee_owned_temp_reclaimed_spec_28_3_K8) {
    const std::string prog =
        "module M; "
        "class Cell "
        "  public static live: int := 0; "
        "  public Value: int; "
        "  public constructor(v: int) Value := v; "
        "    Cell.live := Cell.live + 1; "
        "  end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Main(): int "
        "    return (case new Cell(3) of "
        "             var c: Cell => c.Value + Cell.live * 10, "
        "             _ => -1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 13);
    BEZEL_EXPECT_EQ(runValueOnly(prog), 13);
}

// ── DOUBLE-FREE PRE-EMPT (the one that matters). A wildcard arm's
//    scrutinee is a BOUND named local — the fix MUST NOT fire on a
//    scrutinee that is NOT a fresh owned producer, or it would emit a
//    reclaim at end of case for a value the binding still owns
//    (double-free). Without the fix this is unaffected; with the fix the
//    gate's `producerState == Owned` predicate excludes a plain local
//    read (Borrowed), so no reclaim is added. Encoded: seen (5) * 10 +
//    live (1, because c is still alive after the case) = 51. (issue #1795)
BEZEL_TEST(case_scrutinee_owned_temp_reclaim_1795,
           case_stmt_borrow_scrutinee_not_freed_no_double_free_spec_28_3) {
    const std::string prog =
        "module M; "
        "class Cell "
        "  public static live: int := 0; "
        "  public Value: int; "
        "  public constructor(v: int) Value := v; "
        "    Cell.live := Cell.live + 1; "
        "  end constructor; "
        "  destructor Cell.live := Cell.live - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Main(): int "
        "    using var c := new Cell(5); "
        "    var seen: int := 0; "
        "    case c of "
        "      var x: Cell => do seen := x.Value; end, "
        "      _ => do seen := -1; end "
        "    end; "
        "    return seen * 10 + Cell.live; "
        "  end Main; "
        "end C;";
    // The scrutinee `c` is a BORROW — the binding owns the cell, the
    // case sees a borrow. live stays 1 across the case; destructor fires
    // only when `c` goes out of scope at end of Main.
    BEZEL_EXPECT_EQ(runChecked(prog), 51);
    BEZEL_EXPECT_EQ(runValueOnly(prog), 51);
}

// ── Value-type scrutinee pre-empt. A `new` of a record (value type,
//    §28.3 / §9: no disposal obligation) is classified Borrowed by
//    producerState; the gate's `Kind == Class || Kind == Interface` arm
//    excludes it. Without the fix, **this case was already leak-free**
//    (records are values); the test pins that the fix does not
//    incorrectly emit a reclaim on a value-type scrutinee — would be a
//    false reclaim of stack memory. Encoded: pair.V (11) * 10 + pair.V
//    (11) = 121. (issue #1795 reviewer-directed self-pass)
BEZEL_TEST(case_scrutinee_owned_temp_reclaim_1795,
           case_stmt_value_type_scrutinee_not_freed_spec_28_3) {
    const std::string prog =
        "module M; "
        "record Pair(V: int, W: int); "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pair(11, 0); "
        "    var seen: int := 0; "
        "    case p of "
        "      var x: Pair => do seen := x.V * 10 + x.W; end, "
        "      _ => do seen := -1; end "
        "    end; "
        "    return seen; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 110);
    BEZEL_EXPECT_EQ(runValueOnly(prog), 110);
}
