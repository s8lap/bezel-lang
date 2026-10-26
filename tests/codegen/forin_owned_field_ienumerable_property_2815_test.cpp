// Codegen / SIGSEGV — #2815 / §29.2 / §23.2: a `for var x in obj.P` where
// `P` is a COMPUTED property of declared type `IEnumerable<int>` whose
// getter body returns an `owned Array<int>` FIELD (`get => _items;`) lowered
// the raw `bz_array_t*` instead of a vtable-backed IEnumerable<T>. Every
// consumer of the property read — a local-decl-init (`var it := obj.P;`),
// a direct for-in (`for var x in obj.P`), an argument, etc. — then drove
// §29.2's vtable dispatch on the raw array descriptor and jumped through
// vtable slot 4 (GetEnumerator, NULL) → SIGSEGV (`pc 0x0`, exit 139, no
// leak — a HARD CRASH, not the silent-wrong-value family #1840 closed).
//
// THE FIX. `wrapPropertyArrayAsIEnumerable` (LlvmBackend_Assign.cpp,
// declared in LlvmBackend_impl.hpp) is applied at the EXPRESSION-BODIED
// (§23.2 `=> expr;`) and BARE-IDENTIFIER (`implicit-this P`) property-read
// sites in `lowerExpr` (LlvmBackend_Exprs.cpp) AFTER every other wrap
// (nullable/box) has run. The BLOCK-BODIED sibling has been correct since
// #2703 — the RETURN-arm wrap at `lowerReturn` (wrapAsIEnumerableForTarget)
// wraps an Array/Set/LinkedList/class into a vtable-backed enumerator on the
// way out, so a `get { return _items; }` already produces a proper wrapper.
// The expression-bodied and bare-identifier paths have NO return statement
// to wrap on — the value goes straight from the getter body to the
// property-read expression — so the wrap has to live at the property-read
// site, alongside #2761's boxForTarget. The companion half of the fix is in
// `registerIterableSourceDisposal` (LlvmBackend_Setup.cpp): when the local
// is declared (explicitly OR via inference) as pure `IEnumerable<T>` and
// the init is an `obj.P` MemberAccessExpr whose computed getter body is
// array-backed, claim `ownsSource = true` so the freshly minted wrapper
// frame is disposed at the local's scope end via the slot-1
// iterEnumDispose — otherwise the wrapper leaks (32B / iteration, the
// exact leak the issue's #2751 family catalogued for the auto-property
// path and the same size the existing #2769/#2703 fixes reclaim on the
// block-bodied sibling).
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan; the SEGV on
// the pre-fix path is caught as a non-zero exit code (or ASan's "SEGV on
// unknown address 0x000000000000 (pc 0x0)"). The exit code pins the correct
// enumeration result (20 × (1+2) = 60), so a UAF in the wrapper would
// corrupt the yield values as well. Each of the four cells below names the
// exact §23.2 / §29.2 wiring it exercises so a future ratchet review can
// identify the covered surface. // per §29.2, §23.2, §28.9 (issue #2815)

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// 60 is the issue's repro sum: 20 iterations of `[1, 2]` yields 1 + 2 each,
// repeated 20 times = 60. The exit-code pinning pins the enumeration value
// so a premature free (UAF) would corrupt the yield values as well as flip
// r.ran, matching the #1840 family.
constexpr int kExpectedSum = 60;

}  // namespace

// ---------------------------------------------------------------------------
// // per §29.2, §23.2 (issue #2815)
// ---------------------------------------------------------------------------

// HEADLINE: the issue's repro verbatim. `for var x in it` where `it` is a
// local init'd from `s.Items` (a property whose declared type is
// `IEnumerable<int>` and whose getter body is `=> _items;` where `owned
// _items: Array<int>`). Pre-fix: pc 0x0 SIGSEGV (the property read lowered
// the raw `bz_array_t*` and the for-in's vtable slot-4 (GetEnumerator) read
// the first 8 bytes of the array descriptor as a vtable pointer → NULL →
// SEGV). Post-fix: exit code 60, LSan-clean (the wrapper is disposed at
// the local's scope end via registerIterableSourceDisposal's new arm).
BEZEL_TEST(forin_owned_field_ienumerable_property_2815,
           issue_repro_local_then_for_in_no_segv_29_2) {
    const std::string src =
        "module M; "
        "class Src "
        "  private owned _items: Array<int> := [1, 2]; "
        "  public constructor() end constructor; "
        "  public property Items: IEnumerable<int> "
        "    get => _items; "
        "  end Items; "
        "end Src; "
        "class C public static method Main(): int "
        "  var total := 0; "
        "  var i := 0; "
        "  while i < 20 do "
        "    using var s := new Src(); "
        "    var it := s.Items; "
        "    for var x in it do total := total + x; end for; "
        "    i := i + 1; "
        "  end while; "
        "  return total; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);    // ASan-clean (wrapper frame freed exactly once)
    BEZEL_EXPECT_EQ(r.exitCode, kExpectedSum);  // 20 × (1+2) = 60
}

// DIRECT FOR-IN (no intermediate local): `for var x in s.Items` directly.
// Exercises the lowerIEnumerableBuiltinFor path where the iterable is a
// MemberAccessExpr of a property (the #2769 block-bodied arm handles the
// block-bodied sibling; this is the EXPRESSION-BODIED cell — the wrap
// happens at the property-read site, the for-in's own sourceIsNamedLocal
// classification then sees a LocalSymbol → false for the wrapping wrapper,
// so the for-in's own iterEnumDispose slot frees the wrapper frame).
// Pre-fix: SIGSEGV at slot 4. Post-fix: 60, clean.
BEZEL_TEST(forin_owned_field_ienumerable_property_2815,
           direct_for_in_over_property_no_segv_29_2) {
    const std::string src =
        "module M; "
        "class Src "
        "  private owned _items: Array<int> := [1, 2]; "
        "  public constructor() end constructor; "
        "  public property Items: IEnumerable<int> "
        "    get => _items; "
        "  end Items; "
        "end Src; "
        "class C public static method Main(): int "
        "  var total := 0; "
        "  var i := 0; "
        "  while i < 20 do "
        "    using var s := new Src(); "
        "    for var x in s.Items do total := total + x; end for; "
        "    i := i + 1; "
        "  end while; "
        "  return total; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpectedSum);
}

// EXPLICIT-TYPE LOCAL: `var it: IEnumerable<int> := s.Items;`. Mirrors the
// headline but with an explicit `IEnumerable<int>` annotation on the local —
// exercises registerIterableSourceDisposal's path where `ld->type` is set
// (the inferred-type arm is the headline cell; the explicit arm proves the
// `tr` re-derivation does not double-register when both paths would fire).
// Same crash / same fix; same observable. // per §29.2, §23.2
BEZEL_TEST(forin_owned_field_ienumerable_property_2815,
           explicit_ienumerable_local_then_for_in_no_segv_29_2) {
    const std::string src =
        "module M; "
        "class Src "
        "  private owned _items: Array<int> := [1, 2]; "
        "  public constructor() end constructor; "
        "  public property Items: IEnumerable<int> "
        "    get => _items; "
        "  end Items; "
        "end Src; "
        "class C public static method Main(): int "
        "  var total := 0; "
        "  var i := 0; "
        "  while i < 20 do "
        "    using var s := new Src(); "
        "    var it: IEnumerable<int> := s.Items; "
        "    for var x in it do total := total + x; end for; "
        "    i := i + 1; "
        "  end while; "
        "  return total; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpectedSum);
}

// CONTROL: the BARE-IDENTIFIER spelling of #2815 — `for var x in P` inside
// the declaring class where `P` is the property `=> _items`. Exercises the
// bare-property-read path in LlvmBackend_Exprs.cpp (the qualified-MemberAccess
// and bare-IdentifierExpr arms are two distinct sites; the issue's repro
// exercises the qualified arm, this one exercises the bare arm, both must
// wrap). Same crash / same fix; same observable. // per §29.2, §23.2
BEZEL_TEST(forin_owned_field_ienumerable_property_2815,
           bare_identifier_property_read_in_class_no_segv_29_2) {
    const std::string src =
        "module M; "
        "class Src "
        "  private owned _items: Array<int> := [1, 2]; "
        "  public constructor() end constructor; "
        "  public property Items: IEnumerable<int> "
        "    get => _items; "
        "  end Items; "
        "  public method Sum(): int "
        "    var total := 0; "
        "    for var x in Items do total := total + x; end for; "
        "    return total; "
        "  end Sum; "
        "end Src; "
        "class C public static method Main(): int "
        "  var total := 0; "
        "  var i := 0; "
        "  while i < 20 do "
        "    using var s := new Src(); "
        "    total := total + s.Sum(); "
        "    i := i + 1; "
        "  end while; "
        "  return total; "
        "end Main; end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kExpectedSum);
}
