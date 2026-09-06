// Codegen — #1779 / §21.4 / §29.2: `for var x in xs` where `xs` has STATIC
// type a user interface that extends `IEnumerable<T>`, implemented by TWO OR
// MORE distinct classes with DISTINCT `GetEnumerator` methods (a truly
// polymorphic collection).
//
// Root cause (pre-fix): the #1776 fix devirtualizes the for-in to a direct
// static call when every implementer resolves `GetEnumerator` to the SAME
// method (single implementer, or base + subclasses sharing the inherited
// non-virtual GetEnumerator). When the implementers' `GetEnumerator` are
// genuinely DISTINCT (different bodies, different concrete enumerator types),
// no single static call is sound; the pre-fix code bailed to the generic
// codegen NYI ("not yet implemented: codegen for this for statement") —
// sound (a compile error, never a miscompile), but uninformative: the user
// sees an internal-sounding message with no hint that the polymorphism is
// the reason, and no pointer to the tracked issue or the spec section.
//
// Fix (§29.2): replace the silent fall-through with a SPECIFIC diagnostic
// that names the polymorphic interface, the #1779 root cause (the builtin
// `IEnumerable<T>` is not yet synthesized as an itable-participating
// interface, so no runtime dispatch slot exists), and points at two sound
// workarounds (iterate the concrete class, or share a GetEnumerator).
// The runtime fix (itable dispatch over a builtin IEnumerable descriptor)
// is the documented proper fix in #1779; this test pins the diagnostic so
// the gap is visible to users until that lands.
//
// Diagnostic tests use runAot (reject-test pattern): the compilation MUST
// fail with the specific message. A test that pinned only "compilation
// fails" would be satisfied by the pre-fix generic NYI, so we match the
// SPECIFIC phrase ("#1779", "polymorphic") in addition to asserting that
// the compilation is rejected.

#include "support/aot_result_asserts.hpp"   // fix #3895: diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Shared preamble — two distinct enumerator classes implementing IEnumerator<int>,
// plus a user interface extending IEnumerable<int> with two implementing classes
// each defining their OWN distinct GetEnumerator (returning the respective
// enumerator type). This is the genuinely-polymorphic case from #1779.
const char* kBase =
    "module M; "
    "class IntRangeEnumA: IEnumerator<int> "
    "  private lo: int; private hi: int; private cur: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; cur:=lo-1; end constructor; "
    "  public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end IntRangeEnumA; "
    "class IntRangeEnumB: IEnumerator<int> "
    "  private lo: int; private hi: int; private cur: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; cur:=lo-1; end constructor; "
    "  public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext; "
    "  public property Current: int get => cur*10; end Current; "
    "end IntRangeEnumB; "
    "interface IMyColl: IEnumerable<int> end IMyColl; "
    "class CollA: IMyColl "
    "  protected lo: int; protected hi: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return new IntRangeEnumA(lo, hi); end GetEnumerator; "
    "end CollA; "
    "class CollB: IMyColl "
    "  protected lo: int; protected hi: int; "
    "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return new IntRangeEnumB(lo, hi); end GetEnumerator; "
    "end CollB; ";

}  // namespace

// #1779: a `for` over an interface-typed source whose implementing classes
// define DISTINCT GetEnumerator methods (truly polymorphic) is rejected
// with a diagnostic that mentions #1779 and the polymorphism — the pre-fix
// generic NYI "not yet implemented: codegen for this for statement" does
// not satisfy this test.
BEZEL_TEST(iface_ienumerable_poly_for_1779, polymorphic_source_rejected_with_specific_diag) {
    auto r = runAot(
        std::string(kBase) +
        "class C "
        "  public static method Sum(xs: IMyColl): int "
        "    var t := 0; "
        "    for var x in xs do t := t + x; end; "
        "    return t; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var a := new CollA(1, 3); "
        "    return C.Sum(a); "
        "  end Main; end C;");
    // The compilation must reject the for-in. We don't care which class
    // type we pass (CollA or CollB); either makes the implementer set
    // polymorphic (two distinct GetEnumerators), and either is the same
    // diagnostic from the perspective of the for-in lowering.
    //
    // fix #3895 (issue #3892): the pre-fix generic NYI text would be: "not
    // yet implemented: codegen for this for statement" — without our
    // diagnostic, a bare `hasErrors()`/`!ran` pair (which also cannot
    // distinguish a runtime crash/signal) would still be true. The new
    // diagnostic must mention the issue (#1779) and the polymorphism reason
    // ("polymorphic"); pin both, which subsumes the reject claim.
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "#1779"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "polymorphic"));
}

// #1779: the specific diagnostic is emitted REGARDLESS of which implementer
// is named in Main — the polymorphism is about the IMPLEMENTER SET, not the
// specific value. Both CollA and CollB should produce the same #1779 message
// (a regression guard: a naive fix might inspect only the named value).
BEZEL_TEST(iface_ienumerable_poly_for_1779, polymorphic_source_diag_independent_of_named_value) {
    auto r = runAot(
        std::string(kBase) +
        "class C "
        "  public static method Sum(xs: IMyColl): int "
        "    var t := 0; "
        "    for var x in xs do t := t + x; end; "
        "    return t; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var b := new CollB(1, 3); "  // different implementer than above
        "    return C.Sum(b); "
        "  end Main; end C;");
    // fix #3895 (issue #3892): see polymorphic_source_rejected_with_specific_diag
    // above — pinning the specific #1779 text subsumes the reject claim.
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "#1779"));
}

// #1779 regression guard: when the implementers SHARE a GetEnumerator (a
// base class with the GetEnumerator, subclasses inheriting it), the #1776
// devirtualization still applies and the for-in COMPILES cleanly. The
// diagnostic must NOT fire for the shared-base case — only for genuinely
// polymorphic implementers.
BEZEL_TEST(iface_ienumerable_poly_for_1779, shared_getenumerator_still_devirtualizes) {
    auto r = runAot(
        std::string("module M; "
        "class IntRangeEnum: IEnumerator<int> "
        "  private lo: int; private hi: int; private cur: int; "
        "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; cur:=lo-1; end constructor; "
        "  public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext; "
        "  public property Current: int get => cur; end Current; "
        "end IntRangeEnum; "
        "interface IMyColl: IEnumerable<int> end IMyColl; "
        "class CollBase: IMyColl "
        "  protected lo: int; protected hi: int; "
        "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; end constructor; "
        "  public method GetEnumerator(): IEnumerator<int> return new IntRangeEnum(lo, hi); end GetEnumerator; "
        "end CollBase; "
        "class CollA: CollBase "
        "  public constructor(lo: int, hi: int) base(lo, hi); end constructor; "
        "end CollA; "
        "class CollB: CollBase "
        "  public constructor(lo: int, hi: int) base(lo, hi); end constructor; "
        "end CollB; "
        "class C "
        "  public static method Sum(xs: IMyColl): int "
        "    var t := 0; "
        "    for var x in xs do t := t + x; end; "
        "    return t; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var a := new CollA(1, 3); "
        "    using var b := new CollB(1, 3); "
        "    return C.Sum(a) + C.Sum(b); "  // 6 + 60 = 66 (A's Current*1, B inherits same)
        "  end Main; end C;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_EQ(r.exitCode, 12);  // (1+2+3) + (1+2+3) = 12
}