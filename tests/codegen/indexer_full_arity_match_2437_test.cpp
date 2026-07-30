// fix #2437 (§11.2/§23.4, v0.235): `Emitter::findIndexer`
// (LlvmBackend_Assign.cpp), used by `classIndexerImplementing` to pick which
// concrete indexer on a class fills an interface indexer's itable slot,
// matched candidates by their FIRST index-parameter type ALONE — arity of
// the rest of the parameter list was never checked. §23.4 states "Indexers
// may be overloaded by parameter type list" (the full list, not just the
// first parameter), and §11.2's matching rule requires "the same arity ...
// parameter types that are exactly equal".
//
// CONFIRMED SYMPTOM (the issue's own repro, reproduced here before the fix):
// an interface `[i: int, j: int]: int` obligation, against a class declaring
// BOTH `[i: int]: int => 77` (declared first) and
// `[i: int, j: int]: int => 5 + i + j` (declared second) — reading through
// the interface returned `77` (the 1-parameter overload's constant), not the
// correct `17` (`5 + 1 + 2`) from the 2-parameter overload. Silent wrong
// value, no diagnostic, ASan-clean (a logic bug in candidate SELECTION, not
// a memory-safety defect — ensureIndexerGetterFn/-SetterFn install a real,
// well-typed function for the SELECTED-but-wrong candidate).
//
// The fix (LlvmBackend_Assign.cpp, `findIndexer`) matches the interface
// indexer's FULL parameter list — arity AND every parameter type — mirroring
// `selectIndexer`'s already-correct call-site overload selection (#1172).
// This file exercises the shape as a value/dispatch test (not a reject
// test): every case here compiles and RUNS; the assertion is on the
// returned VALUE. `findIndexer`'s sibling defect (#2442, no base-class walk)
// has its own coverage in missing_iface_indexer_impl_2438_test.cpp
// (inherited_only_satisfaction_accepted_runs_correct_value_2442), landed in
// the same commit as this file's sema-side mirror update.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// The issue's own repro, verbatim shape: a 2-param read-only interface
// obligation, a class overloading `[i:int]` (first-declared, wrong-arity
// candidate the pre-fix code selected) and `[i:int,j:int]` (the correct
// candidate) — GET must reach the 2-param overload. 5+1+2=8, NOT 77.
BEZEL_TEST(indexer_full_arity_match_2437,
           two_param_obligation_selects_two_param_overload_not_first_11_2_23_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public interface IGrid "
        "  readonly property [i: int, j: int]: int; "
        "end IGrid; "
        "public class Grid: IGrid "
        "  public property [i: int]: int => 77; "
        "  public property [i: int, j: int]: int => 5 + i + j; "
        "end Grid; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new Grid(); "
        "    var ig: IGrid := g; "
        "    return ig[1, 2]; "  // must be 8 (5+1+2), not 77
        "  end Main; "
        "end C;"), 8);
}

// Same shape with the overloads declared in the OPPOSITE order (2-param
// first, 1-param second) — proves the fix matches by SHAPE, not by "the
// first-declared indexer is exempt from the arity-blind bug" happenstance.
// NOTE: this specific ordering also passes pre-fix (the old first-param-
// type-only scan happens to hit the 2-param overload first in declaration
// order here), so on its own it does not discriminate pre/post-fix — it is
// a robustness/coverage case, not a fail-without/pass-with pin. The pin for
// #2437 itself is the test above (and the three below it), where the
// wrong-arity overload is declared first and the pre-fix code matches it.
BEZEL_TEST(indexer_full_arity_match_2437,
           declaration_order_reversed_still_selects_correct_overload_23_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public interface IGrid "
        "  readonly property [i: int, j: int]: int; "
        "end IGrid; "
        "public class Grid: IGrid "
        "  public property [i: int, j: int]: int => 5 + i + j; "
        "  public property [i: int]: int => 77; "
        "end Grid; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new Grid(); "
        "    var ig: IGrid := g; "
        "    return ig[10, 20]; "  // must be 35 (5+10+20), not 77
        "  end Main; "
        "end C;"), 35);
}

// READ-WRITE obligation: both GET and SET must dispatch to the 2-param
// overload, not the narrower 1-param one. set ig[3,4] := 100 must reach the
// 2-param setter (storing 100 - 3 - 4 = 93); a subsequent get ig[1,1] must
// read back 93 + 1 + 1 = 95. If either accessor mis-dispatched to the
// 1-param overload, this would either fail to compile (wrong accessor
// shape) or silently ignore/misroute the `j` argument.
BEZEL_TEST(indexer_full_arity_match_2437,
           two_param_readwrite_obligation_dispatches_get_and_set_correctly) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public interface IGrid "
        "  property [i: int, j: int]: int; "
        "end IGrid; "
        "public class Grid: IGrid "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => 999; "
        "    set => _v := -1; "
        "  end; "
        "  public property [i: int, j: int]: int "
        "    get => _v + i + j; "
        "    set => _v := value - i - j; "
        "  end; "
        "end Grid; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new Grid(); "
        "    var ig: IGrid := g; "
        "    ig[3, 4] := 100; "
        "    return ig[1, 1]; "  // 93 + 1 + 1 = 95
        "  end Main; "
        "end C;"), 95);
}

// Three overloads (1, 2, and 3 parameters) sharing the SAME first parameter
// type — the pre-fix code's "match by first param type" would have matched
// whichever of these three came first in declaration order regardless of
// the obligation's actual arity. The 3-param obligation must reach the
// 3-param overload specifically.
BEZEL_TEST(indexer_full_arity_match_2437,
           three_overloads_sharing_first_param_type_selects_by_full_arity) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public interface ICube "
        "  readonly property [i: int, j: int, k: int]: int; "
        "end ICube; "
        "public class Cube: ICube "
        "  public property [i: int]: int => 1; "
        "  public property [i: int, j: int]: int => 10; "
        "  public property [i: int, j: int, k: int]: int => 100 + i + j + k; "
        "end Cube; "
        "public class C "
        "  public static method Main(): int "
        "    using var c := new Cube(); "
        "    var ic: ICube := c; "
        "    return ic[1, 2, 3]; "  // 100+1+2+3 = 106
        "  end Main; "
        "end C;"), 106);
}

// RECORD variant: the same arity-blind selection defect, on a record
// implementing the interface with overloaded indexers (body-form record
// with mutable properties, per §23.4: "Body-form records with mutable
// properties may declare read-write indexers").
BEZEL_TEST(indexer_full_arity_match_2437,
           record_two_param_obligation_selects_two_param_overload) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public interface IGrid "
        "  readonly property [i: int, j: int]: int; "
        "end IGrid; "
        "public record Grid(seed: int): IGrid "
        "  public property [i: int]: int => 77; "
        "  public property [i: int, j: int]: int => 5 + i + j; "
        "end Grid; "
        "public class C "
        "  public static method Main(): int "
        "    var g := new Grid(0); "
        "    var ig: IGrid := g; "
        "    return ig[2, 3]; "  // 5+2+3 = 10, not 77
        "  end Main; "
        "end C;"), 10);
}

// CONTROL: a single, non-overloaded indexer is unaffected by the fix (the
// common case — no overload set to mis-select from).
BEZEL_TEST(indexer_full_arity_match_2437,
           single_indexer_control_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public interface IBox "
        "  readonly property [i: int]: int; "
        "end IBox; "
        "public class Box: IBox "
        "  public property [i: int]: int => 40 + i; "
        "end Box; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var ib: IBox := b; "
        "    return ib[2]; "  // 42
        "  end Main; "
        "end C;"), 42);
}
