// Codegen / soundness — a BOUND block-bodied iterator-format property read
// (`var e := h.Seq; for var x in e do … end;`) claimed the source from the
// mere PRESENCE of a block getter, with no check that a wrapper frame was
// minted. Issue #6405.
//
// Spec (revision v0.338 — current on HEAD):
//   §29.2 "Desugaring of `for`" — the loop binds `using var __source := source;`.
//     "When `source` is a fresh class-typed expression … the wrapping
//     `using var` assumes the disposal obligation"; but "When `source` is
//     itself an owned local — including a `using var` of class type or any
//     other owned class-typed local — the desugaring is adjusted: … replaced
//     by a BORROW that pins `source` for the loop's duration without
//     assuming the disposal obligation. … the obligation stays with the
//     original owner."
//   §28.2 "The compiler-inserted free" — "Every class instance is reclaimed
//     by compiler-inserted code at its **holder's release point** … and
//     nowhere else", the containing object's teardown being the release
//     point for an `owned` field.
//   §23.3 "Block-form properties" — a `get => do … end` accessor is a user
//     statement body; §28.9 adds that the `get => e` and `get => do … end`
//     spellings "are semantically indistinguishable at every read site".
//   §28.3 — the owned-local discharge rule that makes the `for`'s reclaim of
//     a FRESH source an obligation at all.
//
// THE BUG. `registerIterableSourceDisposal` (LlvmBackend_Setup.cpp) computed
// `ownsSource` for a `MemberAccessExpr` initializer as
//
//     if (blockGetterOf(ctx_.exprSymbol(*ma))) ownsSource = true;
//
// — a SYNTACTIC test. Whether a §29.2 wrapper frame exists is not a syntactic
// fact: `wrapAsIEnumerableForTarget` DECLINES whenever the value it is handed
// already carries the declared head, and a block-bodied
//
//     property Seq: IEnumerable<int> get do return _seq; end; end Seq;
//
// over an `owned _seq: IEnumerable<int>` field is exactly that decline. No
// frame is built, the `owned` field keeps its obligation (§28.2), and the
// claim registered the loop's reclamation over the holder's own storage —
// which §29.2 says must be a BORROW, not an obligation-assumption.
//
// MEASURED pre-fix (`src/codegen/LlvmBackend_Setup.cpp` at f6021ee9, the
// commit this test ships beside). `var e := h.Seq; for var x in e do … end;`
// exits 1 under ASan at DEFAULT settings:
//
//   heap-use-after-free … READ of size 8
//     #0 bz$M.HolderI$DisposeCore   #2 main
//   freed by:    bz$arrenumown$ptr$Dispose
//   allocated by: bz$M.HolderI$ctor
//
// — a 32-byte region. The reader is the HOLDER'S OWN DESTRUCTOR reading the
// field the loop already freed: the second reclaim of one object, which §28.2
// admits at the holder's release point "and nowhere else".
//
// THE FIX. Ask the wrap creator what it did, rather than re-deriving it from
// the static shape — the same repair #3419 applied to the expression-bodied
// sibling and #6397 applied to the direct `for x in h.E` consumer. The block
// getter's return edge publishes its own `wrapAsIEnumerableForTarget`
// `outWrapMade` in `propBlockGetterReturnKinds_` under
// `kPropGetterReturnMint`, keyed on the PropertyDecl, set-or-CLEARED per edge.
// So CLAIM = MINT by construction, and a decline erases the verdict instead
// of leaving a stale claim.
//
// TEST PLAN.
//   (A) THE DECLINE — `owned _seq: IEnumerable<int>` field, block getter
//       returning it, bound to a local and iterated. Pre-fix: exit 1, ASan
//       heap-use-after-free. Post-fix: exit 25, ASan/LSan clean. Sixteen
//       iterations so a single accidental reclaim would be caught on more
//       than one holder, and so the row's `total` is not satisfied by one
//       lucky pass.
//   (B) THE MINT — the #2769 shape, a block getter over an `owned
//       OwningArray<Item>` field whose body return NEEDS the §29.2 wrap.
//       This is the direction a narrowing can get wrong: if the new mint test
//       were over-tight the frame would stop being claimed and this row would
//       go from exit 25 to a 32-byte-per-read leak. Pre- and post-fix alike:
//       exit 25, clean. It is also the row that proves the fix is a
//       narrowing of an over-claim and not a deletion of the arm.
//   (C) THE DIRECT SPELLING — `for var x in h.Seq do … end;` over the (A)
//       DECLINE shape. That path is #6397's `lowerForIn`
//       `sourceIsNamedLocal` arm, which already consults the same published
//       verdicts; it must keep classifying the field read as a lend. Both
//       before and after this fix: exit 25, clean. Its job is to catch a fix
//       that broke the consumer to fix the claimant.
//
// NOT COVERED HERE (deliberately, and reported with the fix): the
// `using var e := h.Seq;` TWIN of (A), which reaches the same syntactic
// claim through `registerUsingDisposal`'s own `ownsSource` arm and its
// `usingVarSourceIsBorrow` override — two further sites in the same file,
// with their own borrow-pin blast radius. It reproduces the same
// heap-use-after-free and is left for a separate change.

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// (A) + (C) preamble — the DECLINE shape. `Array<int>` (not
// `OwningArray<Item>`) backs the field deliberately, for a reason that is a
// MEASUREMENT and not a preference: with a class-element owning collection
// the `owned _seq: IEnumerable<Item>` store mints a wrap whose cascade does
// not reach the elements, and `new Item(3)` / `new Item(4)` leak 2 × 16 bytes
// in the HOLDER'S CONSTRUCTOR ALONE — verified by a probe that never reads
// the property (`using var h := new HolderI(); return 25;`), so it is
// independent of this fix and would otherwise be attributed to it. With a
// value-element `Array<int>` the whole program is leak-free, which is what
// lets row (A) be a leak oracle as well as a UAF oracle.
const char* kDeclineHolder =
    "module M; "
    "class HolderI "
    "  private owned _seq: IEnumerable<int>; "
    "  public constructor() "
    "    var a := new Array<int>(); "
    "    a.Add(3); "
    "    a.Add(4); "
    "    _seq := a; "
    "  end constructor; "
    "  public property Seq: IEnumerable<int> "
    "    get => do "
    "      return _seq; "
    "    end; "
    "  end Seq; "
    "end HolderI; ";

// (B) preamble — the MINT shape. The #2769 / #2703 owning-field oracle,
// byte-compatible with
// tests/codegen/block_bodied_ienumerable_property_bound_disposal_2769_test.cpp
// so a failure here is comparable with that file's rows (A)/(B).
const char* kMintHolder =
    "module M; "
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

// (A) BOUND + ITERATED over the DECLINE shape. 16 × (3+4) = 112. // §29.2
const char* kBoundOverDecline =
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..16 do "
    "      using var h := new HolderI(); "
    "      var e := h.Seq; "
    "      for var x in e do total := total + x; end; "
    "    end; "
    "    if total = 112 then return 25; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// (C) DIRECT spelling over the same DECLINE shape. // §29.2
const char* kDirectOverDecline =
    "class P "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..16 do "
    "      using var h := new HolderI(); "
    "      for var x in h.Seq do total := total + x; end; "
    "    end; "
    "    if total = 112 then return 25; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// (B) BOUND + ITERATED over the MINT shape. 16 × (3+4) = 112. // §29.2
const char* kBoundOverMint =
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

}  // namespace

// =========================================================================
// (A) THE DECLINE — the issue's repro. The wrap never minted, so nothing may
// be claimed, and the holder's destructor must find its own field live.
// pre-fix:  exit 1, ASan heap-use-after-free (READ of size 8, 32 bytes) in
//           bz$M.HolderI$DisposeCore, freed by bz$arrenumown$ptr$Dispose.
// post-fix: exit 25, ASan/LSan clean.
// // per §29.2 / §28.2 / §28.3 / §23.3, v0.338
// =========================================================================
BEZEL_TEST(codegen_block_bodied_declining_wrap_no_source_claim_6405,
           bound_var_over_declining_block_getter_leaves_the_holders_field_alive_spec_29_2) {
    JitResult r = runAotChecked(std::string(kDeclineHolder) + kBoundOverDecline);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 25);
}

// =========================================================================
// (B) THE MINT — the narrowing must not withdraw a claim from a frame the
// wrap really built. Pre- and post-fix: exit 25, clean. A regression here is
// a LEAK (one 32-byte wrapper frame per read), which is the safe direction
// but still a defect, so it is pinned.
// // per §29.2 / §28.3 / §23.3
// =========================================================================
BEZEL_TEST(codegen_block_bodied_declining_wrap_no_source_claim_6405,
           bound_var_over_minting_block_getter_still_reclaims_the_wrapper_spec_29_2) {
    JitResult r = runAotChecked(std::string(kMintHolder) + kBoundOverMint);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 25);
}

// =========================================================================
// (C) THE DIRECT SPELLING over the same DECLINE shape — #6397's
// `sourceIsNamedLocal` consumer must keep calling it a lend.
// // per §29.2 / §28.2
// =========================================================================
BEZEL_TEST(codegen_block_bodied_declining_wrap_no_source_claim_6405,
           direct_for_in_over_declining_block_getter_still_treats_it_as_a_lend_spec_29_2) {
    JitResult r = runAotChecked(std::string(kDeclineHolder) + kDirectOverDecline);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 25);
}