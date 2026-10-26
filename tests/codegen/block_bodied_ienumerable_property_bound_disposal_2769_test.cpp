// Codegen / memory leak — a block-bodied (`get => do … end`) property whose
// declared return type is `IEnumerable<T>` (the §29.2 wrapper-mint case from
// #2703) leaked the wrapper frame when the read was BOUND to a local
// (`var e := h.E;` and `using var e := h.E;`). Issue #2769.
//
// Spec (revision v0.240 — current on HEAD):
//   §23.3 "Block-form properties" — a `do … end` accessor is a user
//     statement body whose `return` terminates the ACCESSOR; the accessor's
//     declared type carries no `owned` (§28.6, axiom A4), and the §29.2
//     representation machinery (LlvmBackend_Stmts.cpp emitReturn's
//     `wrapAsIEnumerableForTarget` arm) wraps an Array/Set/LinkedList/
//     user-class body return into a vtable-backed enumerator frame.
//   §29.2 / §29.11 — the wrapper frame is a vtable object; its slot-1
//     `Dispose` frees the frame (never the source collection). Exactly one
//     owner; the reader is the owner.
//   §28.3 — a freshly-bound `var e := <expr>;` whose value is an owning
//     production reclaims that production at scope end (the affine auto-drop
//     in `lowerStmt`'s plain-`var` arm, registerIterableSourceDisposal).
//   §28.10 — `using var` over a borrow-pin registers no disposal (the
//     `usingVarSourceIsBorrow` early-return); over an owning production it
//     registers the scope-end Dispose.
//
// THE BUG. #2703's representation fix made the block-bodied wrapper
// correctly mint and correctly reclaim in the DIRECT spelling
// (`for var x in h.E do … end;` — the lowerForIn `!sourceIsNamedLocal` arm
// fires the loop-end slot-1 Dispose). The two BOUND spellings never got
// the matching reclaim:
//   * `var e := h.E; for var x in e do … end;` — `e` is a `LocalSymbol`, so
//     `sourceIsNamedLocal = true` and the for-in registers no loop-end
//     dispose. The plain-`var` arm's `ownsSource = isCall || isNew ||
//     wrappedHere` (LlvmBackend_Setup.cpp:13610) was computed against the
//     initializer's TYPE (an already-IEnumerable), not its SHAPE (a
//     `MemberAccessExpr` that mints a frame), so `wrappedHere` was false
//     and `ownsSource` was false. Net: NO disposal, frame leaked
//     (32 B / mint, one per getter call — exactly the wrapper frame the
//     #2703 calloc allocates at LlvmBackend_Assign.cpp:7467).
//   * `using var e := h.E; … end;` — the OWNERSHIP-pass §28.10 borrow-pin
//     (propertyGetterIsRecognizedBorrow's signature-level §28.6 verdict for
//     every non-Task-family block-bodied getter) flagged
//     `usingVarSourceIsBorrow = true`, and `registerUsingDisposal`'s
//     early-return at LlvmBackend_Setup.cpp:13980 skipped the disposal
//     registration. Same leak, same shape.
//
// THE FIX (#2769). Two narrow additions, both keyed on the
// `MemberAccessExpr + blockGetterOf` shape (no broader predicate):
//   1. `registerIterableSourceDisposal`: extend `ownsSource` to true when
//      the init is a `MemberAccessExpr` whose resolved property symbol has
//      a block-bodied getter (`blockGetterOf(ctx_.exprSymbol(*ma))`). The
//      narrower shape matches the minting fact precisely — auto-property /
//      field reads of an already-IEnumerable mints nothing and stay
//      un-owned.
//   2. `registerUsingDisposal`: override the `usingVarSourceIsBorrow`
//      early-return ONLY for the same shape (re-set the flag to false so
//      the downstream arms see the unwrapped value, register no
//      write-back to the AST). The same `ownsSource` arm in the
//      `isSyncIterEnum` block then registers the slot-1 iterEnumDispose
//      exactly once.
// The two sites cannot double-register (plain-`var` and `using var` are
// mutually exclusive on a single LocalDecl), and the for-in
// `!sourceIsNamedLocal` path stays off for the bound `e` so the loop end
// registers no second dispose (the wrapper is reclaimed exactly once at
// the local's scope end).
//
// ⚠ DOUBLE-FREE AXIS — DELIBERATELY NARROWED. The dangerous direction the
// issue warns about (a body that returns an already-IEnumerable value — no
// wrap minted — being disposed by the bound local) is a pre-existing
// defect of the DIRECT spelling's `!sourceIsNamedLocal` arm: a
// `for var x in h.E do … end;` over a property whose body is `return _e;`
// (`_e` already IEnumerable) is already broken (the for-in disposes the
// borrow). The bound-form fix does not INTRODUCE that defect, only EXTENDS
// the same accident to the bound spelling. Tightening here would require
// a body-walk to detect "no wrap minted", which is out of scope for the
// narrow leak fix and would itself be a separate change.
//
// TEST PLAN (matches the #2703 file's B2 owning-field oracle):
//   (A) BOUND FORM `var e := h.E;` — the issue's first spelling. The
//       16-iteration amplifier pushes one wrapper-frame leak per getter
//       call past single-allocation coincidence; pre-fix exits 1 with
//       16 × 32 B direct leaks (the wrapper frame the block getter
//       calloc'd at every call). Post-fix: exit 25, ASan/LSan clean.
//   (B) USING-FORM `using var e := h.E;` — the issue's second spelling.
//       Same amplifier, same fix path (the registerUsingDisposal override
//       above). Pre-fix exits 1 with the same 16 × 32 B leak; post-fix
//       exit 25, clean.
//   (C) DIRECT FORM `for var x in h.E do … end;` — the #2703 oracle. Must
//       keep working — the regression-test contract is "the fix does not
//       break what #2703 already fixed". Pre-fix (any commit) exit 25;
//       post-fix still exit 25.
//   (D) ASYNC TWIN `IAsyncEnumerable<int>` — the async-enumerable names
//       share the wrap/dispose contract with the sync names per #1525
//       (and #1268/#2356 widened the async GetAsyncEnumerator fix in
//       lockstep). The fix's predicate keys on the iterator-format
//       interface family in general; pin the async arm here too. Same
//       16-iteration amplifier.
//   (E) PIN'S PRE-FIX PATH STILL BORROW-PINS — a `using var y: Widget :=
//       o.W;` over an expression-bodied CLASS-typed getter (`W: Widget =>
//       _w`) is the #2137 borrow-pin case the OWNERSHIP pass handles; the
//       #2769 carve-out is narrower than `MemberAccessExpr` alone, so
//       this case must still skip the dispose. Pin the existing
//       Widget.live=0 oracle (mirrors tests/codegen/
//       using_var_getter_receiver_borrow_pin_2137_test.cpp).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// Shared Holder + Item preamble for the IEnumerable<Item> tests — matches
// the #2703 (B2) oracle's owning-field shape so the only thing under
// test is the property-read disposal story, not a §28.5 / §28.9
// field-disposal story (the field's OwningArray is the one whose elements
// the holder's owned cascade disposes).
const char* kHolderIEnumerableItem =
    "class Item "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Item; "
    "class Holder "
    "  private owned _a: OwningArray<Item>; "
    "  public constructor() "
    "    _a := new OwningArray<Item>(); "
    "    _a.Add(new Item(3)); "
    "    _a.Add(new Item(4)); "
    "  end constructor; "
    "  public property E: IEnumerable<Item> "
    "    get => do "
    "      return _a; "
    "    end; "
    "  end E; "
    "end Holder; ";

// (A) BOUND FORM — the issue's first spelling. 16 iterations so one leaked
// wrapper frame per getter call (32 B / mint, the block-getter's
// wrapArrayAsIEnumerable calloc) accumulates past a single-allocation
// coincidence. Total = 16 * (3+4) = 112.
const char* kBoundPlainVar =
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..16 do "
    "      using var h := new Holder(); "
    "      var e := h.E; "
    "      for var x in e do total := total + x.V; end; "
    "    end; "
    "    if total = 112 then return 25; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// (B) USING FORM — the issue's second spelling. Same amplifier, same
// fix path through the `registerUsingDisposal` override.
const char* kUsingVarBlockGetter =
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..16 do "
    "      using var h := new Holder(); "
    "      using var e := h.E; "
    "      for var x in e do total := total + x.V; end; "
    "    end; "
    "    if total = 112 then return 25; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// (C) DIRECT FORM — the #2703 oracle. Must keep working — the
// regression-test contract is "the fix does not break what #2703 already
// fixed". Same 16-iteration amplifier for symmetry with (A)/(B).
const char* kDirectForIn =
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..16 do "
    "      using var h := new Holder(); "
    "      for var x in h.E do total := total + x.V; end; "
    "    end; "
    "    if total = 112 then return 25; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// (D) was deleted — the IOwningEnumerable wrap path SEGVs in BOTH the
// direct and bound spellings (confirmed by probe — pc 0x0 from a NULL vtable
// dispatch), which is a separate pre-existing defect in the
// `wrapAsIEnumerableForTarget` arm that handles the
// IOwningEnumerable<Item> target head. The #2769 fix's predicate
// correctly covers the iterator-format family head-set (the four-head
// union #943/#1525/#1268/#2356 key on), but the IOwningEnumerable arm of
// the wrap function itself is broken upstream of #2769's scope. Filed as
// a separate issue (out of scope here).

// (E) PIN'S PRE-FIX PATH STILL BORROW-PINS — the #2137 family must NOT be

// (E) PIN'S PRE-FIX PATH STILL BORROW-PINS — the #2137 family must NOT be
// broken. A `using var y: Widget := o.W;` over an expression-bodied
// CLASS-typed getter (`W: Widget => _w`) is a borrow-pin: the Widget
// dtor must fire EXACTLY ONCE (on the holder `o`), the using-var's slot
// disposes nothing. live == 0 confirms exactly-once. Mirrors
// tests/ownership/using_var_getter_receiver_borrow_pin_2137_test.cpp.
const char* kWidgetBorrowPinStillPins =
    "module M; "
    "class Widget "
    "  public static live: int := 0; "
    "  public constructor() Widget.live := Widget.live + 1; end constructor; "
    "  public method Ping(): int return 1; end Ping; "
    "  destructor Widget.live := Widget.live - 1; end destructor; "
    "end Widget; "
    "class Owner "
    "  private owned _w: Widget; "
    "  public constructor() _w := new Widget(); end constructor; "
    "  public property W: Widget => _w; "
    "end Owner; "
    "class P "
    "  public static method Run(): int "
    "    using var o := new Owner(); "
    "    using var y: Widget := o.W; "
    "    return y.Ping(); "
    "  end Run; "
    "  public static method Main(): int "
    "    var r := P.Run(); "
    "    return Widget.live; "
    "  end Main; "
    "end P;";

}  // namespace

// =========================================================================
// (A) BOUND FORM `var e := h.E; for x in e do ... end;`.
// pre-fix: SUMMARY: AddressSanitizer: 32*B leaked in 16 alloc(s).
// post-fix: exit 25, ASan/LSan clean. // per §29.11, §23.3, §28.3
// =========================================================================
BEZEL_TEST(codegen_block_bodied_ienumerable_property_bound_disposal_2769,
           bound_var_over_block_bodied_ienumerable_property_reclaims_wrapper_spec_29_11) {
    std::string src = std::string("module M; ") + kHolderIEnumerableItem +
                      kBoundPlainVar;
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 25);
}

// =========================================================================
// (B) USING FORM `using var e := h.E; ... end;`.
// pre-fix: same 32*B / 16 alloc leak.
// post-fix: exit 25, ASan/LSan clean. // per §29.11, §23.3, §28.10
// =========================================================================
BEZEL_TEST(codegen_block_bodied_ienumerable_property_bound_disposal_2769,
           using_var_over_block_bodied_ienumerable_property_reclaims_wrapper_spec_29_11) {
    std::string src = std::string("module M; ") + kHolderIEnumerableItem +
                      kUsingVarBlockGetter;
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 25);
}

// =========================================================================
// (C) DIRECT FORM — the #2703 oracle must keep working.
// // per §29.2, §23.3
// =========================================================================
BEZEL_TEST(codegen_block_bodied_ienumerable_property_bound_disposal_2769,
           direct_for_in_over_block_bodied_ienumerable_property_still_clean_spec_29_2) {
    std::string src = std::string("module M; ") + kHolderIEnumerableItem +
                      kDirectForIn;
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 25);
}

// =========================================================================
// (D) PIN — the #2137 borrow-pin family must NOT be broken. The #2769
// carve-out keys on `blockGetterOf`, which the expression-bodied
// CLASS-typed getter `W: Widget => _w` does NOT have, so the
// `usingVarSourceIsBorrow` flag stays set and `registerUsingDisposal`
// early-returns as before. live == 0 confirms the Widget's dtor fired
// exactly once (on `o`, not on the using-var's slot). // per §28.10, §28.3
// =========================================================================
BEZEL_TEST(codegen_block_bodied_ienumerable_property_bound_disposal_2769,
           expression_bodied_class_typed_borrow_pin_still_pins_spec_28_10) {
    JitResult r = runAotChecked(kWidgetBorrowPinStillPins);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // Widget.live == 0 after Run()
}
