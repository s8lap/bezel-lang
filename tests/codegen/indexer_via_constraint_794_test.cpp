// fix #794 / §14.2 / §23.4: indexing a value whose static type is a generic
// type PARAMETER constrained to an interface that declares an indexer
// (`x[i]` where `x: T`, `where T: IIndexable`). Before the fix the access
// returned 0: `indexReceiverDecl` bailed on the null declaration of a bare type
// parameter, so `lowerIndexerGet`/`Set` never reached the concrete indexer.
//
// The fix resolves the type parameter through the active monomorphization
// (`genericSubstSemaTy_`) to its concrete class/record declaration — mirroring
// the method-call constraint devirtualization (§14.2). It also fixed a latent
// bug in the expression-bodied indexer inline path: the index ARGUMENT was
// lowered after the caller's locals/params were cleared, so an index that
// referenced an enclosing local/parameter (here the method parameter `i`)
// resolved to 0 — exercised by the non-generic regression test below.
//
// These are codegen (dispatch/value) tests — runJit is the right harness.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// fix #2417 (§11.2/§23.4, v0.235): `readonly` — every use of `iface` below
// (`class_arg_getter`, `record_arg_getter`, `two_instantiations`) implements
// with a getter-only expression-bodied indexer and only ever reads through
// the interface; a read-write obligation here is the getter-only-satisfying-
// read-write shape #2417's accessor-set conformance check now rejects at
// compile time. `setter_through_constraint` below needs (and keeps) its own
// separate, genuinely read-write `IBox` interface, since `Cell` provides a
// real setter and the test exercises it.
const char* iface =
    "module M; "
    "interface IIndexable readonly property [i: int]: int; end IIndexable; ";
}  // namespace

// §14.2 / §23.4: the canonical #794 repro — a CLASS argument indexed through a
// generic constraint. 40 + 2 = 42 (was 0).
BEZEL_TEST(indexer_via_constraint_794, class_arg_getter) {
    BEZEL_EXPECT_EQ(run(
        std::string(iface) +
        "class SingleInt: IIndexable "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property [i: int]: int => _v + i; "
        "end SingleInt; "
        "class C "
        "  public static method At<T>(x: T, i: int): int where T: IIndexable "
        "    return x[i]; "
        "  end At; "
        "  public static method Main(): int "
        "    using var s := new SingleInt(40); "
        "    return At<SingleInt>(s, 2); "      // 42
        "  end Main; "
        "end C;"), 42);
}

// §14.2 / §9.4 / §23.4: a RECORD argument indexed through a generic constraint.
// The record receiver is a by-value struct and is spilled to a pointer for the
// getter's `this`. 40 + 2 = 42 (was malformed IR before the spill fix).
BEZEL_TEST(indexer_via_constraint_794, record_arg_getter) {
    BEZEL_EXPECT_EQ(run(
        std::string(iface) +
        "record RecInt(v: int): IIndexable "
        "  public property [i: int]: int => v + i; "
        "end RecInt; "
        "class C "
        "  public static method At<T>(x: T, i: int): int where T: IIndexable "
        "    return x[i]; "
        "  end At; "
        "  public static method Main(): int "
        "    var r := new RecInt(40); "
        "    return At<RecInt>(r, 2); "         // 42
        "  end Main; "
        "end C;"), 42);
}

// §14.2 / §23.4: indexer SET through a generic constraint. Cell starts at 0;
// `x[5] := 30` runs `_v := value + i` = 35; then `x[5]` get returns _v = 35.
BEZEL_TEST(indexer_via_constraint_794, setter_through_constraint) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IBox property [i: int]: int; end IBox; "
        "class Cell: IBox "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property [i: int]: int "
        "    get => _v; "
        "    set => _v := value + i; "
        "  end; "
        "end Cell; "
        "class C "
        "  public static method SetAt<T>(x: T, i: int, val: int): int "
        "      where T: IBox "
        "    x[i] := val; "
        "    return x[i]; "
        "  end SetAt; "
        "  public static method Main(): int "
        "    using var c := new Cell(0); "
        "    return SetAt<Cell>(c, 5, 30); "    // 35
        "  end Main; "
        "end C;"), 35);
}

// §14.2: two distinct instantiations (class + record) in one program both
// devirtualize to their own indexers. (40+2) + (100+3) = 145.
BEZEL_TEST(indexer_via_constraint_794, two_instantiations) {
    BEZEL_EXPECT_EQ(run(
        std::string(iface) +
        "class SingleInt: IIndexable "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property [i: int]: int => _v + i; "
        "end SingleInt; "
        "record RecInt(v: int): IIndexable "
        "  public property [i: int]: int => v + i; "
        "end RecInt; "
        "class C "
        "  public static method At<T>(x: T, i: int): int where T: IIndexable "
        "    return x[i]; "
        "  end At; "
        "  public static method Main(): int "
        "    using var s := new SingleInt(40); "
        "    var r := new RecInt(100); "
        "    return At<SingleInt>(s, 2) + At<RecInt>(r, 3); "   // 42 + 103 = 145
        "  end Main; "
        "end C;"), 145);
}

// §23.4 regression (latent, exposed by #794): a NON-generic expression-bodied
// indexer whose index references the enclosing method's PARAMETER. The index
// `idx` is now lowered before the getter-inline clears the caller's params.
// 40 + 2 = 42.
BEZEL_TEST(indexer_via_constraint_794, nongeneric_index_is_caller_param) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int) "
        "  public property [i: int]: int => x + i; "
        "end Pt; "
        "class C "
        "  public static method Get(p: Pt, idx: int): int "
        "    return p[idx]; "
        "  end Get; "
        "  public static method Main(): int "
        "    var p := new Pt(40); "
        "    return Get(p, 2); "                // 42
        "  end Main; "
        "end C;"), 42);
}
