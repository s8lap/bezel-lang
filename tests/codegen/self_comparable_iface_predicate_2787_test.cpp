// #2787 — the §14.2-rule-3 self-recurrence predicate ("does this interface name
// `IComparable<Self>`?") now has ONE definition, `sema::selfComparableIface`.
//
// It previously existed in three independent copies: codegen's
// `selfComparableIfaceCompareTo` (#2768, reused by #2770 and #2782), an inline
// scan in sema's Sort/Sorted gate (#2768), and sema's `selfComparableIfaceArg`
// (#2782).  That is the shape of a defect this project has already paid for
// once — an earlier run found three form sites each computing a loop depth with
// `outerDepth`, needed two separate fixes (#2556, #2566) and then a sweep.
//
// WHAT THIS FILE PINS, and why it is not a vacuous test for a pure refactor.
// The factoring's whole value is that the three CALL SITES cannot drift apart,
// so that is what is asserted directly: one interface shape is driven through
// all three gates that consult the predicate —
//     Array<T>.Sort()        (stdlib §4.4)
//     Array<T>.Min()/.Max()  (stdlib §5.1)
//     Math.Min/Max           (stdlib §2.2)
// — and their answers are pinned, both on the shape they ADMIT and on the shape
// they REJECT.  Re-inlining any one copy and then changing it would break these
// cases even though every per-issue test above still passed.
//
// ⚠ The first thing this test caught is that they do NOT all agree today:
// `Sort()` accepts an invariance-violating element that the other two reject.
// That is pre-existing (measured identical either side of this refactor) and is
// filed as #2790; the case below pins the divergence with its issue number
// rather than asserting a uniformity that does not yet hold.
//
// This matters concretely and soon: #2776 records that the predicate does NOT
// reach a CROSS-MODULE interface (an imported interface's `baseTypeRefs` does
// not carry its `IComparable` base).  Whoever fixes that must have it apply to
// every caller at once, which is now automatic — and these cases fail loudly if
// a future change makes it true of only one gate.
//
// No "fails without the fix" demonstration accompanies this file: #2787 is a
// behaviour-preserving refactor, and the evidence for it is that the seven
// dependent fixes' own suites (#2768, #2770, #2782, #2784, #2785, #2786, #2788 —
// 64 cases) stay green.  Inventing a case that "fails before" would mean
// inventing a behaviour change there is none of.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
bool rejects(const std::string& program) {
    auto r = runAot(program);
    return !r.ran && r.diagnostics.hasErrors();
}

// The ADMITTED shape: `interface IFoo: IComparable<IFoo>` — §14.2 rule 3's
// self-recurrence, exactly what `where T: IComparable<T>` asks for.
const char* kAdmitted =
    "module M; "
    "public interface IFoo: IComparable<IFoo> "
    "  method Rank(): int; "
    "end IFoo; "
    "class Node: IFoo "
    "  private _r: int; "
    "  public constructor(r: int) _r := r; end constructor; "
    "  public method Rank(): int => _r; "
    "  public method CompareTo(other: IFoo): int => _r - other.Rank(); "
    "end Node; ";

// The REJECTED shape: `interface IG<T>: IComparable<IG<int>>` used at
// `IG<string>`.  §12.9 — IComparable is INVARIANT — so the self-recurrence is
// not satisfied.  This is the case a predicate weakened to
// `arg->declaration == d` would wrongly admit, which is why every copy compared
// the argument with typeRefEqual.
const char* kRejected =
    "module M; "
    "public interface IG<T>: IComparable<IG<int>> "
    "  method Rank(): int; "
    "end IG; "
    "class G<T>: IG<T> "
    "  public constructor() end constructor; "
    "  public method Rank(): int => 1; "
    "  public method CompareTo(other: IG<int>): int => 0; "
    "end G; ";

std::string prog(const char* preamble, const std::string& body) {
    return std::string(preamble) +
           "class C  public static method Main(): int " + body +
           "  end Main; end C; ";
}
}  // namespace

// GATE 1 — stdlib §4.4 `Array<T>.Sort()`.  Ranks 7,3 ascending: sorted order is
// 3 then 7, packed as arr[0]*10 + arr[1] = 37.
BEZEL_TEST(self_comparable_iface_predicate_2787, gate_sort_admits_self_comparable) {
    BEZEL_EXPECT_EQ(runChecked(prog(kAdmitted,
        "    using var a := new Node(7); "
        "    using var b := new Node(3); "
        "    var arr: Array<IFoo> := [a, b]; "
        "    arr.Sort(); "
        "    return arr[0].Rank() * 10 + arr[1].Rank(); ")), 37);
}

// GATE 2 — stdlib §5.1 `Array<T>.Min()/.Max()`.  Same 37, deliberately: the
// three gates are asserted to produce the SAME answer for the SAME shape.
BEZEL_TEST(self_comparable_iface_predicate_2787, gate_array_minmax_admits_self_comparable) {
    BEZEL_EXPECT_EQ(runChecked(prog(kAdmitted,
        "    using var a := new Node(7); "
        "    using var b := new Node(3); "
        "    var arr: Array<IFoo> := [a, b]; "
        "    return arr.Min().Rank() * 10 + arr.Max().Rank(); ")), 37);
}

// GATE 3 — stdlib §2.2 `Math.Min`/`Math.Max`.
BEZEL_TEST(self_comparable_iface_predicate_2787, gate_math_minmax_admits_self_comparable) {
    BEZEL_EXPECT_EQ(runChecked(prog(kAdmitted,
        "    using var a := new Node(7); "
        "    using var b := new Node(3); "
        "    var x: IFoo := a; "
        "    var y: IFoo := b; "
        "    return Math.Min(x, y).Rank() * 10 + Math.Max(x, y).Rank(); ")), 37);
}

// ...and the same three gates must agree on what the predicate DECLINES.
// §12.9 invariance: `IG<string>` does not satisfy `IComparable<IG<int>>`.

// ⚠ SORT DOES NOT AGREE — and this case pins that divergence rather than
// hiding it.  `arr.Sort()` COMPILES AND RUNS on the shape the other two gates
// reject (exit 7).  MEASURED to be byte-identical before and after #2787's
// refactor, so it is pre-existing and not caused by the factoring; filed as
// **#2790**.  Under generated-code ASan it also runs clean, so it is not
// currently an observable memory error — but `ensureIfaceCmpThunk`'s own header
// records that the accepting path's fallback is "`bz$strcmp` on a vtable
// global, i.e. this issue's own unbounded read, which happens not to fault only
// because a vtable typically contains an early zero byte."
//
// Asserting the CORRECT behaviour here would leave a red test in the suite;
// asserting nothing would let the divergence pass unrecorded.  So the current
// behaviour is pinned with its issue number: when #2790 is fixed this case will
// fail, and that failure is the signal to flip it to `rejects(...)` and delete
// this comment.
BEZEL_TEST(self_comparable_iface_predicate_2787, gate_sort_diverges_pinned_2790) {
    BEZEL_EXPECT_EQ(runChecked(prog(kRejected,
        "    using var a := new G<string>(); "
        "    using var b := new G<string>(); "
        "    var arr: Array<IG<string>> := [a, b]; "
        "    arr.Sort(); "
        "    return 7; ")), 7);
}

BEZEL_TEST(self_comparable_iface_predicate_2787, gate_array_min_declines_invariance_violation) {
    BEZEL_EXPECT_TRUE(rejects(prog(kRejected,
        "    using var a := new G<string>(); "
        "    using var b := new G<string>(); "
        "    var arr: Array<IG<string>> := [a, b]; "
        "    var m := arr.Min(); "
        "    return 7; ")));
}

BEZEL_TEST(self_comparable_iface_predicate_2787, gate_math_min_declines_invariance_violation) {
    BEZEL_EXPECT_TRUE(rejects(prog(kRejected,
        "    using var a := new G<string>(); "
        "    using var b := new G<string>(); "
        "    var x: IG<string> := a; "
        "    var y: IG<string> := b; "
        "    var m := Math.Min(x, y); "
        "    return 7; ")));
}
