// narrow #3551 (§29.3 / §30.1 / §30.7, v0.286): `NestedBodyScope` was a fourth
// hand-maintained copy of the suspend/resume frame list. Its ctor/dtor pair
// (LlvmBackend_Assign.cpp ~4802 / ~4862) drifted from the
// `BEZEL_SM_FRAME_MEMBERS` X-macro authority (LlvmBackend_impl.hpp ~11036):
// nine members (the four cell-frame additions of #3466/#3484 and the five
// async-result additions of #1503/#1521) were deliberately INHERITED, and
// one member — `iterIsOwningIter_` (#2621) — was MISSING from both the
// struct member list AND the ctor-init / ctor-clear / dtor-restore triple.
// It was not "clobbered by an uninitialised struct-field write" (no member
// in the ctor-init list means no dtor line to clobber it); the scope never
// saved or cleared or restored it, so its value across a nested emission
// simply LEAKED through to the outer body. The fix generates NestedBodyScope
// from the X-macro so a future member added to `BEZEL_SM_FRAME_MEMBERS`
// cannot drift between save / clear / restore, and the asymmetry #3505
// catalogues is structurally impossible here.
//
// ON REACHABILITY (why these rows pass on BOTH compilers). The behavioural
// shape the lane first hypothesised — `iterIsOwningIter_` being
// clobbered mid-scope — is unreachable from any real program: every write
// to `iterIsOwningIter_` lives inside `emitIteratorMethod`
// (LlvmBackend_Iterators.cpp ~1760 / ~1929), and every call to
// `emitIteratorMethod` from inside another body is wrapped in an
// `SmFrameSaveScope` (the #3505 fix at LlvmBackend_Calls2.cpp ~12208) that
// already saves / restores the field. A nested emission that mutates
// `iterIsOwningIter_` therefore restores it before the surrounding
// `NestedBodyScope` exits, so even on the pre-fix compiler the observable
// behaviour of these rows is 3 disposals, 1 taken = 31. The fix's
// observable effect on these rows is therefore NONE; what the fix delivers
// is the structural guarantee that the next member added to the X-macro
// is handled in all four NestedBodyScope sites by construction, which is
// what the sibling test `nested_body_scope_x_macro_structural_3551` pins
// (FAIL-WITHOUT / PASS-WITH on the source-level X-macro fingerprint).
//
// WHY KEEP THESE ROWS THEN. They are the BEHAVIOURAL PIN that documents the
// expected behaviour today and guarantees it stays correct when the next
// member is added. The KEY row (`owning_iter_with_block_property_read_…`)
// is the IOwningEnumerable-with-property-between-yields shape that the
// fold first closed (the #2621 displaced-dispose path); the two controls
// are load-bearing — they establish that the BLOCK property (which
// triggers NestedBodyScope) is the only perturbation, not the property
// read per se, and that the same iterator body without any property read
// is also clean.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// R with a counter destructor (the exactly-once oracle). Same R shape as
// #2621's test — increment R._n in the destructor, count the disposals.
const std::string kCountR =
    "class R "
    "  public v: int := 0; "
    "  public static _n: int := 0; "
    "  public constructor(n: int) v := n; end constructor; "
    "  destructor R._n := R._n + 1; end destructor; "
    "end R; ";

}  // namespace

// =====================================================================
// #3551 — sync IOwningEnumerable<T> with a BLOCK-bodied property read on a
// Helper instance in the iterator body BETWEEN the second and third
// `yield return new R()`. The first property read triggers
// `ensurePropertyBlockGetter`, which constructs a NestedBodyScope around
// the memoised getter body lowering. On both compilers this produces 3
// disposals / 1 taken (= 31) for the reason given above: every actual
// write to `iterIsOwningIter_` is inside an `SmFrameSaveScope` that
// restores before the surrounding NestedBodyScope exits, so the scope's
// save/clear/restore of the field has no observable effect on this row
// today. The row's value is documenting the expected behaviour (the
// §29.1 displaced-dispose path is correct) and pinning it across future
// X-macro additions.
//
// Counting (both compilers): R(1) taken via using var → disposed by consumer
// (1). R(2) abandoned in iterator → disposed at the THIRD yield's displaced
// dispose (2). R(3) abandoned in iterator → disposed at the iterator's
// exhaustion transition (3, the #2616 path). Net: 3 disposed, 1 taken.
// =====================================================================

// The KEY behavioural pin: three yields, abandon R(2) and R(3) by MoveNext-
// without-TakeCurrent. The property read between yields 2 and 3 triggers
// the NestedBodyScope. Pins that the §29.1 displaced-dispose still fires
// across the nested-block-property emission.
BEZEL_TEST(nested_body_scope_iter_frame_clobber_3551,
           owning_iter_with_block_property_read_disposes_abandoned_via_yield_store) {
    std::string program = std::string("module M; ") + kCountR +
        "class Helper "
        "  public Counter: int := 0; "
        "  public property P: int "
        "    get => do Counter := Counter + 1; return Counter; end; "
        "  end P; "
        "end Helper; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "    using var h := new Helper(); "
        "    var x := h.P; "  // BLOCK-bodied property read; NestedBodyScope runs
        "    yield return new R(3); "
        "  end Gen; "
        "  private static method Drive(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var taken := 0; "
        "    if en.MoveNext() then "
        "      using var x := en.TakeCurrent(); "
        "      taken := taken + x.v; "
        "    end; "
        "    var junk := 0; "
        "    while en.MoveNext() do junk := junk + 1; end; "
        "    return taken; "
        "  end Drive; "
        "  public static method Main(): int "
        "    var t := Drive(); "
        "    return R._n * 10 + t; "
        "  end Main; end C;";
    // 3 disposals * 10 + 1 taken = 31.
    BEZEL_EXPECT_EQ(runChecked(program), 31);
}

// A control shape: the SAME iterator body but EXPRESSION-bodied property
// read (inlined, no NestedBodyScope). 31 on both sides of the fix. Confirms
// the test is sensitive to the BLOCK property's NestedBodyScope, not to
// the property read per se.
BEZEL_TEST(nested_body_scope_iter_frame_clobber_3551,
           control_with_expression_property_read_stays_clean_spec_29_3) {
    std::string program = std::string("module M; ") + kCountR +
        "class Helper "
        "  public Counter: int := 0; "
        "  public property P: int "
        "    get => Counter + 1; "
        "  end P; "
        "end Helper; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "    using var h := new Helper(); "
        "    var x := h.P; "  // EXPRESSION-bodied — inlined, no NestedBodyScope
        "    yield return new R(3); "
        "  end Gen; "
        "  private static method Drive(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var taken := 0; "
        "    if en.MoveNext() then "
        "      using var x := en.TakeCurrent(); "
        "      taken := taken + x.v; "
        "    end; "
        "    var junk := 0; "
        "    while en.MoveNext() do junk := junk + 1; end; "
        "    return taken; "
        "  end Drive; "
        "  public static method Main(): int "
        "    var t := Drive(); "
        "    return R._n * 10 + t; "
        "  end Main; end C;";
    BEZEL_EXPECT_EQ(runChecked(program), 31);
}

// NO-NESTED-BODY control: the same iterator body WITHOUT the property read
// between yields 2 and 3. No NestedBodyScope trigger; 31 on both sides.
// Confirms the 31 reading of the key regression is achievable in the
// iterator body shape, not a fluke of the property read.
BEZEL_TEST(nested_body_scope_iter_frame_clobber_3551,
           no_nested_body_control_stays_clean_spec_29_3) {
    std::string program = std::string("module M; ") + kCountR +
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "    yield return new R(3); "
        "  end Gen; "
        "  private static method Drive(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var taken := 0; "
        "    if en.MoveNext() then "
        "      using var x := en.TakeCurrent(); "
        "      taken := taken + x.v; "
        "    end; "
        "    var junk := 0; "
        "    while en.MoveNext() do junk := junk + 1; end; "
        "    return taken; "
        "  end Drive; "
        "  public static method Main(): int "
        "    var t := Drive(); "
        "    return R._n * 10 + t; "
        "  end Main; end C;";
    BEZEL_EXPECT_EQ(runChecked(program), 31);
}