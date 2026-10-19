// Codegen — #6447 / §28.9 + §28.2 (v0.338). The BLOCK-bodied getter's
// field-lend verdict, on every spelling of "the getter hands the reader a
// field read" that the previous one-hop test did not cover.
//
// THE DEFECT #6397 CLOSED, AND THE HOLE LEFT IN IT. #6397 taught the two
// for-in CONSUMER sites in src/codegen/LlvmBackend_Stmts.cpp that a property
// read is a lend ("a getter never transfers a reclamation obligation to its
// reader", §28.9) and gave them two places to ask. `computedGetterOf` answers
// for the EXPRESSION-bodied spelling, because such a getter is inlined at the
// read site and its body expression is right there. The BLOCK-bodied spelling
// was left to `propBlockGetterReturnKinds_`, whose `kPropGetterReturnField` bit
// `lowerReturn` writes once per return edge — and that writer's own test is
//
//     dynamic_cast<sema::FieldSymbol*>(ctx_.exprSymbol(*rs->value)) != nullptr
//
// which is ONE HOP. `get do return _a; end` answers it. `get do var t := _a;
// return t; end` does not: `rs->value` names a LOCAL, and the fact that the
// local was bound to a field read lives in the local's initializer, one hop
// further down. So the bit stayed clear, `fieldLend && !minted` was false, and
// `!sourceIsNamedLocal` registered the loop-end reclamation on the holder's own
// `owned` field.
//
// MEASURED at this fix's parent (`7a0c320f`), `bezelc --sanitize=address`, on
// the row programs below:
//
//   ==NNNN==ERROR: AddressSanitizer: heap-use-after-free on address 0x…,
//   READ of size 8 at 0x… thread T0
//       #0 … in bz$TM.HolderX$DisposeCore
//       #1 … in bz$TM.HolderX$Dispose
//       #2 … in main
//   freed by thread T0 here:
//       #0 … in free
//       #1 … in main                      <- the loop-end disposal, inlined
//   previously allocated by thread T0 here:
//       #1 … in bz$TM.HolderX$ctor
//   SUMMARY: AddressSanitizer: heap-use-after-free … in bz$TM.HolderX$DisposeCore
//
// WHY THE FIX IS A BODY QUESTION AND NOT A THIRD BIT. Provenance through a
// body is a syntactic property of the body, so it must not depend on WHICH
// lowering face emitted it — and the map cannot supply that. The vtable/itable
// face (`ensurePropertyAccessorFn`) lowers a block getter body and publishes
// no per-return verdict at all, so a map-keyed `false` there is the ABSENCE of
// an answer, not an answer of "not a field". MEASURED: the DIRECT block
// spelling — the shape #6397 did close — still UAF'd on both arms as soon as
// the property was declared `virtual`, which no per-return-edge publisher can
// ever reach. `blockGetterReturnsFieldLend` (LlvmBackend_Assign.cpp) therefore
// asks the accessor's own `stmts`, face-independently, the same way
// `computedGetterOf` asks the expression body's face-independently.
//
// THE DIRECTION. Every caller feeds the verdict into `fieldLend && !minted`
// → `sourceIsNamedLocal`, and `!sourceIsNamedLocal` is what registers the
// loop-end reclamation. So a WRONG `true` can only cost a leak, never a
// use-after-free — the same asymmetry the #6397 comment records for the mint.

#include <cstdlib>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// 200 iterations: the holder is constructed and torn down inside the loop, so
// the loop-end free and the holder's teardown read are 200 distinct events
// rather than one, and a single-shot repro cannot see the sanitizer's noise
// under them. `hi` bounds SelfA to 1+2+3 = 6, so a fully correct run of the
// laundering shapes prints `s=1200`.
const char* kSelfA =
    "public class SelfA: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end SelfA; ";

// The shared driver. `%s` is spliced with the holder's class name, so every
// row below runs the identical 200-iteration loop and the identical read
// `h.Seq`, and differs ONLY in the getter body under test. That is what keeps
// the rows comparable: a verdict that moved could only have moved because of
// the getter spelling.
const char* kMain =
    "  public static method Main(): int "
    "    var s := 0; "
    "    for var i in 0..<200 do "
    "      var h := new %s(); "
    "      for var x in h.Seq do s := s + x; end; "
    "    end for; "
    "    Console.WriteLine($\"s={s}\"); "
    "    return 0; "
    "  end Main; ";

std::string prog(const char* holderDecl, const char* holderType) {
    std::string m = kMain;
    m.replace(m.find("%s"), 2, holderType);
    return "module TM; " + std::string(kSelfA) + holderDecl +
           "public class TMImpl " + m + "end TMImpl;";
}

// The field is declared as the USER CLASS, so `for var x in h.Seq` selects
// `lowerIEnumerableFor` (the user-class arm).
const char* kSelfAGetterLaundered =
    "public class HolderA "
    "  private owned _a: SelfA; "
    "  public constructor() _a := new SelfA(3); end constructor; "
    "  public property Seq: SelfA "
    "    get => do var t := _a; return t; end; "
    "  end Seq; "
    "end HolderA; ";

// The field is declared `IEnumerable<int>`, so the read is an iterator-format
// property read and the loop selects `lowerIEnumerableBuiltinFor`. Different
// declared-type spelling, DIFFERENT LOWERING FUNCTION, same §28.9 question —
// which is why every row below comes in both spellings.
const char* kIfaceGetterLaundered =
    "public class HolderB "
    "  private owned _b: IEnumerable<int>; "
    "  public constructor() _b := new SelfA(3); end constructor; "
    "  public property Seq: IEnumerable<int> "
    "    get => do var t := _b; return t; end; "
    "  end Seq; "
    "end HolderB; ";

// The local is declared WITHOUT an initializer and bound by a later `:=`. The
// defining operation is then an ASSIGNMENT rather than the decl initializer,
// so a fix that read only `LocalDecl::initializer` would still miss this one.
const char* kSelfAGetterAssignedLocal =
    "public class HolderF "
    "  private owned _a: SelfA; "
    "  public constructor() _a := new SelfA(3); end constructor; "
    "  public property Seq: SelfA "
    "    get => do var t: SelfA; t := _a; return t; end; "
    "  end Seq; "
    "end HolderF; ";

// TWO hops: `u` is bound to a local, which is bound to the field.
const char* kIfaceGetterTwoHops =
    "public class HolderG "
    "  private owned _b: IEnumerable<int>; "
    "  public constructor() _b := new SelfA(3); end constructor; "
    "  public property Seq: IEnumerable<int> "
    "    get => do var t := _b; var u := t; return u; end; "
    "  end Seq; "
    "end HolderG; ";

// CONTROL — THE REASSIGNMENT ESCAPE, and the row that says the fix did not
// over-reach. The local starts on the field and is then OVERWRITTEN with a
// fresh value, so the value the getter returns is NOT the holder's storage and
// the loop's reclamation is still the right answer. A fix that followed the
// local's INITIALIZER without asking what else defines it would classify this
// as a lend, withdraw the loop's reclamation, and convert this row's clean run
// into a 200-allocation leak. MEASURED green on BOTH sides (`s=200`: the
// returned SelfA(1) yields exactly one element per iteration).
//
// Note this shape is the issue reporter's objection #2 verbatim, and it is
// what the "every definition is a field read" rule in
// `blockGetterReturnsFieldLend` exists to answer.
const char* kSelfAGetterReassigned =
    "public class HolderH "
    "  private owned _a: SelfA; "
    "  public constructor() _a := new SelfA(3); end constructor; "
    "  public property Seq: SelfA "
    "    get => do var t := _a; t := new SelfA(1); return t; end; "
    "  end Seq; "
    "end HolderH; ";

// CONTROL — the EXPRESSION-bodied spelling #6397 already closed, kept so that
// a regression in the existing path cannot hide behind the new rows.
const char* kSelfAGetterExpression =
    "public class HolderI "
    "  private owned _a: SelfA; "
    "  public constructor() _a := new SelfA(3); end constructor; "
    "  public property Seq: SelfA get => _a; end Seq; "
    "end HolderI; ";

// CONTROL — THE §29.2 WIDENING. The getter launders a field AND its body
// returns a value that is not the property's declared type, so §29.2's
// wrap-creator mints a FRAME the reader owns and the holder must not. This is
// the row that caught this fix's own first draft: an earlier version of
// `blockGetterReturnsFieldLend` claimed the lend here, and because the
// vtable face publishes no mint at all (`gk == 0` measured) the `!minted`
// term could not override it, so the loop withdrew its reclamation of a frame
// it owns — MEASURED on that draft:
//
//   SUMMARY: AddressSanitizer: 6400 byte(s) leaked in 200 allocation(s)
//
// with the correct `s=1200` printed first. Green on BOTH the parent and the
// fix, because the fix's §29.2 widening guard declines this shape and the
// parent reached the same answer by its one-hop test. It is here so that a
// future narrowing of that guard cannot reintroduce the leak silently.
const char* kSelfAWideningVirtualGetter =
    "public class HolderJ "
    "  private owned _items: Array<int>; "
    "  public constructor() "
    "    _items := new Array<int>(); _items.Add(1); _items.Add(2); _items.Add(3); "
    "  end constructor; "
    "  public virtual property Seq: IEnumerable<int> "
    "    get => do var t := _items; return t; end; "
    "  end Seq; "
    "end HolderJ; ";

JitResult checked(const std::string& p) { return runAotChecked(p); }

}  // namespace

// ===========================================================================
// ROWS 1-2 — THE REPORTED SHAPE, BOTH DISPATCH ARMS.
// `get do var t := _a; return t; end` over an `owned` field. RED at the
// parent on both arms with the heap-use-after-free quoted at the top of this
// file; green with the fix, printing `s=1200` and exiting 0.
// ===========================================================================
BEZEL_TEST(for_in_block_getter_field_lend_6447,
           user_class_typed_laundered_getter_loop_must_not_reclaim_spec_28_9) {
    JitResult r = checked(prog(kSelfAGetterLaundered, "HolderA"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 × (1 + 2 + 3) — the loop genuinely ran, so a row that never
    // enumerated could not report this.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

BEZEL_TEST(for_in_block_getter_field_lend_6447,
           ienumerable_typed_laundered_getter_loop_must_not_reclaim_spec_28_9) {
    JitResult r = checked(prog(kIfaceGetterLaundered, "HolderB"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// ===========================================================================
// NOT PINNED HERE — THE `virtual` POSITION, WHICH IS STILL OPEN.
//
// The shape matrix found that the DIRECT block return #6397 closed faults
// again as soon as the property is declared `virtual`, and so does the
// laundering one. Nothing about the body changes; what changes is that the
// accessor is emitted on the vtable/itable face
// (`ensurePropertyAccessorFn`), which lowers the body WITHOUT setting
// `currentAccessorPropSym_`, so `lowerReturn` publishes nothing there and the
// map holds no entry (`mapEntry=0`, measured) — the reader cannot tell "not a
// field" from "nobody answered".
//
// These rows are deliberately ABSENT rather than `DISABLED_`: they still fault,
// so a row asserting they are clean would be red, and a row asserting the
// fault would pin the bug as expected behaviour. Closing them means publishing
// §29.2's mint on that face, which `kPropGetterReturnMint`'s second reader
// (the #6405 arm of `registerIterableSourceDisposal`) makes a separate change
// with its own oracle. Tracked on issue #6487; see the face guard at
// `blockGetterReturnsFieldLend` for the measurement.
// ===========================================================================

// ===========================================================================
// ROWS 6-7 — OTHER WAYS THE BODY REACHES THE FIELD.
//
// Row 6 defines the local by ASSIGNMENT, not by a decl initializer, so the
// rule has to read `AssignmentStmt`s as definitions too.
// Row 7 chains two locals, so the rule has to follow a local to another local
// rather than stopping at the first hop — which is the whole defect.
// ===========================================================================
BEZEL_TEST(for_in_block_getter_field_lend_6447,
           declared_then_assigned_local_getter_spec_28_9) {
    JitResult r = checked(prog(kSelfAGetterAssignedLocal, "HolderF"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

BEZEL_TEST(for_in_block_getter_field_lend_6447,
           two_hop_laundered_getter_ienumerable_arm_spec_28_9) {
    JitResult r = checked(prog(kIfaceGetterTwoHops, "HolderG"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// ===========================================================================
// CONTROL — THE REASSIGNMENT ESCAPE. Green on BOTH sides, and it is the row
// that fails if the fix reads a local's initializer without also asking what
// ELSE defines that local. Its `s=200` is the tell: `SelfA(1)` yields one
// element per iteration, and 200 iterations that ran to completion is what
// distinguishes "the loop reclaimed the fresh value it owned" from "the loop
// skipped reclamation and the value leaked".
// ===========================================================================
BEZEL_TEST(for_in_block_getter_field_lend_6447,
           reassigned_local_getter_still_reclaims_spec_28_9) {
    JitResult r = checked(prog(kSelfAGetterReassigned, "HolderH"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=200") != std::string::npos);
}

// ===========================================================================
// CONTROL — the EXPRESSION-bodied spelling #6397 closed. Green on both sides.
// ===========================================================================
BEZEL_TEST(for_in_block_getter_field_lend_6447,
           expression_bodied_getter_still_does_not_reclaim_spec_28_9) {
    JitResult r = checked(prog(kSelfAGetterExpression, "HolderI"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// ===========================================================================
// CONTROL — the §29.2 widening. Green on BOTH sides, and the row that caught
// this fix's own first draft leaking 6400 bytes here. See the constant's
// comment for the measurement and for why the guard lives in the predicate
// rather than in the mint publication.
// ===========================================================================
BEZEL_TEST(for_in_block_getter_field_lend_6447,
           widening_laundered_virtual_getter_still_reclaims_spec_29_2) {
    JitResult r = checked(prog(kSelfAWideningVirtualGetter, "HolderJ"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The run is CORRECT AND COMPLETE — a row that merely avoided the fault
    // by not enumerating would also print nothing, so the sum is asserted.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}
