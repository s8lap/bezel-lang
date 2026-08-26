// Codegen / sema lifetime — #3383 (Cycle 5 W23 Lane A): literal-temp Add
// to a borrowing-family collection field/local is a STORED-BORROW hazard
// the spec records as admitted-with-warning per §15.4 / §28.3 / §28.11:
//
//   "the compiler **must warn** when a store-position argument on a
//    **borrowing-family** collection — `Add`/`Insert` and the indexer-set,
//    key and value positions alike — is a **fresh unbound owned temporary
//    of class type**" (§15.4, v0.243; rationale §5.28).
//
// `_s.Add(new Item(5))` where `_s` is an owned field of borrowing
// collection type (Set<Item> / Array<Item> / LinkedList<Item>) and `Item`
// is a class is the canonical hazard: the constructor's scope-end disposal
// frees the temp Item at the §28.3 K8 boundary, but `_s` (a
// borrow-pointer-storing container that outlives the constructor scope)
// keeps the dangling pointer, and any later read via `h[0]` (the indexer
// case Lane B chased) — or `h._s` directly, or a local `for x in s do`
// over the field's borrowed view — dereferences the freed Item (UAF under
// ASan; garbage under plain builds).
//
// The named-local form IS already rejected today (#1886 §28.11 store rule):
//
//   `using var ownTmp := new Item(5); _s.Add(ownTmp);`
//   -> error: "borrow of `ownTmp` passed to `.Add(...)` on `_s` would
//      outlive its source" (§28.11 borrowing-collection store rule)
//
// but the literal-temporary form (the day-to-day `Add(new …)` shape)
// BYPASSES that rejection: the `new Item(N)` IS the same owned scope-bound
// temporary the named-local rule catches, but the NewExpr passes directly
// into `.Add` and is not wrapped in a LocalSymbol, so the existing
// rejection's predicate misses it. The runtime then frees the Items at
// the ctor scope end while `_s` keeps dangling borrow pointers.
//
// Lane B's wrap-primitive hypothesis (#3364) was REFUTED on HEAD e9bd762:
// (1) the bug manifests equally without the indexer (direct field read,
// local var), (2) without the wrap (pre-#3356), and (3) on the non-Set
// borrowing family (Array<Item> shows the same UAF). The wrap primitive
// for `wrapPropertyArrayAsIEnumerable` is structurally correct; the bug
// is in the compiler lifetime analysis.
//
// THE FIX (per spec §15.4, not per the W22-D brief). The spec EXPLICITLY
// records the literal-only-rejection asymmetry:
//
//   "The rejection of non-trivial fresh producers is deliberately
//    **literal-only** (recorded): `Add`, `Insert`, and the indexer-set on
//    a borrowing collection remain governed by §28.3's unbound-temporary
//    admission and §28.11's residual-trust store rule — an opaque
//    callee's signature type-announces no retention — so
//    `coll.Add(new Connection("a"))` stays admissible and stays the
//    borrow-lifetime hazard that checked disposal mode backstops."
//
// So the program STAYS ADMITTED, the hazard is backstopped by checked
// disposal mode (§28.2 — `DisposedAccessException` on later use), and the
// compiler MUST warn (§15.4's mandatory lint). The named-local rejection
// path is untouched; the new path ADDS a warning emission alongside.
//
// TEST PLAN. Each of the unsafe patterns below MUST compile cleanly
// (program admitted) AND must emit a §15.4 fresh-unbound-temp WARNING at
// the `Add` call site. Pre-fix the code compiles cleanly and runs into
// the UAF; the warning assertion therefore fails on the pre-fix compiler.
// The positive control at the bottom asserts the SAFE `using var tmp; …`
// form still compiles + runs to its expected sum (35) without the warning
// — the fix must not over-correct.
//
// // per spec §15.4 (v0.243 mandatory lint, rationale §5.28), §28.3
//    (unbound owned temporary), §28.11 (borrowing-collection store
//    rule / residual-trust store), §28.2 (checked-disposal backstop).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// §15.4 fresh-unbound-temp warning: "fresh unbound owned temporary of
// class type passed to `.Add(...)` on `_s` — the temporary dies at its
// K8 full-expression boundary and the borrowing collection retains a
// reference to the freed instance". The test asserts the SPECIFIC
// warning fires (not any warning, and not just "no errors"). Pre-fix
// no warning fires → assertion fails; post-fix the warning fires →
// assertion passes.
bool emitsFreshUnboundTempWarning(const std::string& body) {
    auto r = runAot(
        std::string("module M; class Item "
                    "  public V: int := 0; "
                    "  public constructor(v: int) V := v; end constructor; "
                    "end Item; ") +
        body);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("fresh unbound owned temporary") !=
                std::string::npos &&
            d.message.find("class type") != std::string::npos)
            return true;
    }
    return false;
}

// Convenience: run to completion and return exit code, or -999 on driver
// failure (compile error, link failure). Used by the positive-control case.
int runExit(const std::string& body) {
    auto r = runAot(
        std::string("module M; class Item "
                    "  public V: int := 0; "
                    "  public constructor(v: int) V := v; end constructor; "
                    "end Item; ") +
        body);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── UNSAFE: literal-temporary Add to a borrowing-collection field/local ──
//
// Pre-fix: compiles cleanly → ctor scope-end frees the Items → main reads
//          a dangling Item → UAF (ASan exit 1; plain exit 70
//          OverflowException for Array<Item>, exit 197 garbage for
//          Set<Item>).
// Post-fix: compiles cleanly (program ADMITTED per §15.4's literal-only
//           asymmetry) AND emits the §15.4 fresh-unbound-temp warning at
//           each `Add(new Item(N))` call site. The hazard is backstopped
//           by checked disposal mode (§28.2); in unchecked mode the
//           pre-fix UAF persists, exactly as the spec records.

// (1) The headline Set<Item> INDEXER case from #3364 — Lane B's repro.
BEZEL_TEST(borrow_temp_into_borrowing_collection_field_3383,
           set_field_indexer_add_temp_warning_fires) {
    BEZEL_EXPECT_TRUE(emitsFreshUnboundTempWarning(
        "class Holder "
        "  private owned _s: Set<Item>; "
        "  public constructor() "
        "    _s := new Set<Item>(); "
        "    _s.Add(new Item(5)); "
        "    _s.Add(new Item(10)); "
        "    _s.Add(new Item(20)); "
        "  end constructor; "
        "  public property [i: int]: IEnumerable<Item> "
        "    => _s; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  var total := 0; "
        "  for var x in h[0] do total := total + x.V; end; "
        "  if total = 35 then return 58; end; "
        "  return total; "
        "end Main; end C;"));
}

// (2) Same pattern with `Array<Item>` — refutes the "Set-specific" framing.
BEZEL_TEST(borrow_temp_into_borrowing_collection_field_3383,
           array_field_indexer_add_temp_warning_fires) {
    BEZEL_EXPECT_TRUE(emitsFreshUnboundTempWarning(
        "class Holder "
        "  private owned _a: Array<Item>; "
        "  public constructor() "
        "    _a := new Array<Item>(); "
        "    _a.Add(new Item(5)); "
        "    _a.Add(new Item(10)); "
        "    _a.Add(new Item(20)); "
        "  end constructor; "
        "  public property [i: int]: IEnumerable<Item> "
        "    => _a; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  var total := 0; "
        "  for var x in h[0] do total := total + x.V; end; "
        "  if total = 35 then return 58; end; "
        "  return total; "
        "end Main; end C;"));
}

// (3) Same pattern WITHOUT the indexer — refutes the "indexer wrap" framing.
//     `_s` is the field; `for x in h.Items` goes through the plain
//     property-read wrap site, the wrap helper's Current returns the same
//     dangling pointer, same UAF.
BEZEL_TEST(borrow_temp_into_borrowing_collection_field_3383,
           set_field_property_add_temp_warning_fires) {
    BEZEL_EXPECT_TRUE(emitsFreshUnboundTempWarning(
        "class Holder "
        "  private owned _s: Set<Item>; "
        "  public constructor() "
        "    _s := new Set<Item>(); "
        "    _s.Add(new Item(5)); "
        "    _s.Add(new Item(10)); "
        "    _s.Add(new Item(20)); "
        "  end constructor; "
        "  public property Items: IEnumerable<Item> "
        "    => _s; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  var total := 0; "
        "  for var x in h.Items do total := total + x.V; end; "
        "  if total = 35 then return 58; end; "
        "  return total; "
        "end Main; end C;"));
}

// (4) Literal-temp Add to a LOCAL borrowing Set — no field, no indexer.
//     Refutes the "field-only" framing.
BEZEL_TEST(borrow_temp_into_borrowing_collection_field_3383,
           local_set_add_temp_warning_fires) {
    BEZEL_EXPECT_TRUE(emitsFreshUnboundTempWarning(
        "class C public static method Main(): int "
        "  var s := new Set<Item>(); "
        "  s.Add(new Item(5)); "
        "  s.Add(new Item(10)); "
        "  s.Add(new Item(20)); "
        "  var total := 0; "
        "  for var x in s do total := total + x.V; end; "
        "  if total = 35 then return 58; end; "
        "  return total; "
        "end Main; end C;"));
}

// ── POSITIVE CONTROL: safe patterns must still compile + run, no warning ──
//
// The sema fix must NOT over-correct: the safe forms (named-owned locals
// outliving the borrow site, or owning-family collections with explicit
// ownership transfer) must continue to work AND must NOT emit the
// §15.4 fresh-unbound-temp warning (the named-local source is not a
// fresh unbound temporary, and the owning family takes ownership).

// (5) OwningSet<Item> with literal-temp `Add(owned …)`-equivalent: the
//     literal `new Item(N)` is added to an OWNING collection, so the
//     items are owned by the collection, not the ctor scope. The
//     borrowing-collection lint does NOT fire on the owning family.
BEZEL_TEST(borrow_temp_into_borrowing_collection_field_3383,
           owningset_field_indexer_add_temp_passes_no_warning) {
    auto r = runAot(
        "class Holder "
        "  private owned _s: OwningSet<Item>; "
        "  public constructor() "
        "    _s := new OwningSet<Item>(); "
        "    _s.Add(new Item(5)); "
        "    _s.Add(new Item(10)); "
        "    _s.Add(new Item(20)); "
        "  end constructor; "
        "  public property [i: int]: IEnumerable<Item> "
        "    => _s; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  var total := 0; "
        "  for var x in h[0] do total := total + x.V; end; "
        "  if total = 35 then return 58; end; "
        "  return total; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 58);
    // No §15.4 fresh-unbound-temp warning on an owning-collection Add.
    bool sawUnexpected = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("fresh unbound owned temporary") !=
            std::string::npos) {
            sawUnexpected = true;
            break;
        }
    }
    BEZEL_EXPECT_FALSE(sawUnexpected);
}

// (6) Borrowing Set<Item> but each Item outlives the Set via `using var` —
//     the named-local form the existing §28.11 rule already admits and
//     the borrow-store rejection does not fire on (each Item's source
//     outlives the receiver). The §15.4 fresh-unbound-temp lint also
//     does NOT fire: the argument is a named-local borrow, not a fresh
//     unbound temporary.
BEZEL_TEST(borrow_temp_into_borrowing_collection_field_3383,
           named_local_overlapping_set_passes_no_warning) {
    auto r = runAot(
        "class C public static method Main(): int "
        "  using var a := new Item(5); "
        "  using var b := new Item(10); "
        "  using var cc := new Item(20); "
        "  using var s: Set<Item> := new Set<Item>(); "
        "  s.Add(a); s.Add(b); s.Add(cc); "
        "  var total := 0; "
        "  for var x in s do total := total + x.V; end; "
        "  if total = 35 then return 58; end; "
        "  return total; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 58);
    bool sawUnexpected = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("fresh unbound owned temporary") !=
            std::string::npos) {
            sawUnexpected = true;
            break;
        }
    }
    BEZEL_EXPECT_FALSE(sawUnexpected);
}
