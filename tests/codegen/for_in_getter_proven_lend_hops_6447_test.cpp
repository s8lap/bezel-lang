// Codegen — #6447 / §28.9 + §28.2 + §29.2 (v0.357). The for-in consumer's
// "the getter hands the reader a holder's own storage" verdict, on the
// spellings that reach the field through MORE than one hop.
//
// WHAT WAS STILL OPEN. The first #6447 fix (`narrow`, v0.338) taught the two
// for-in consumer sites in src/codegen/LlvmBackend_Stmts.cpp to follow a
// block-bodied getter's LOCALS back to a field read. It stopped there: its
// statement walk admitted only top-level `return` / local bindings / `:=`,
// and its expression test admitted only a field or a local. So a getter that
// reached the field through anything else kept no lend claim, `fieldLend &&
// !minted` stayed false, and `!sourceIsNamedLocal` registered the loop-end
// reclamation on the holder's own `owned` field:
//
//   * a `return` under a branch          get do var t := _a;
//                                               if k > 0 then return t; end;
//                                               return t; end
//   * a hop through ANOTHER PROPERTY     get => Inner            (Inner => _a)
//   * the same hop, block-bodied         get do return Inner; end
//   * a hop through a METHOD             get => Get()     (Get(): SelfA => _a)
//
// MEASURED at this fix's parent (`a44c7bec8`), `bezelc --sanitize=address`,
// on every row below marked RED:
//
//   ==NNNN==ERROR: AddressSanitizer: heap-use-after-free on address 0x…
//   READ of size 8 at 0x… thread T0
//       #0 … in bz$TM.HolderX$DisposeCore
//   SUMMARY: AddressSanitizer: heap-use-after-free … in bz$TM.HolderX$DisposeCore
//
// THE RULE. §28.9: "A property or indexer read is a lend for every declared
// type … a getter never transfers a reclamation obligation to its reader, and
// the two accessor body spellings of §23.3 (`get => e` and `get => do … end`)
// are semantically indistinguishable at every read site". §28.2 keeps the
// holder's release point the only place its field is reclaimed. So the
// consumer asks ONE question of the getter's body, whatever its spelling and
// however many hops it takes (`Emitter::propertyGetterIsProvenLend`): is every
// value it can return a proven lend — a field, a local every one of whose
// definitions is a proven lend, a read of another property that lends, or a
// call whose plain class return §28.3's announcement table (row 4) makes "a
// receiver-sourced borrow" — reached without an edge that can mint a §29.2
// frame?
//
// THE MIRROR. A lend claim withdraws the loop's reclamation, so a claim made
// where §29.2 really minted a frame is a LEAK. The control rows at the bottom
// pin the three hop shapes in which a frame IS minted on the way (the inner
// property widens; the outer return widens; a typed local widens) and the
// reassignment shape — each must keep printing its sum with a clean exit,
// exactly as it does at the parent.

#include <cstdlib>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// `hi` bounds SelfA to 1+2+3 = 6.
const char* kSelfA =
    "public class SelfA: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end SelfA; ";

// One driver for every row: 200 iterations, the holder constructed and torn
// down inside the loop, so the loop-end free and the holder's teardown read
// are 200 distinct events and a leaked frame is 200 allocations. A fully
// correct run prints `s=1200`.
const char* kMain =
    "public class TMImpl "
    "  public static method Main(): int "
    "    var s := 0; "
    "    for var i in 0..<200 do "
    "      var h := new Holder(); "
    "      for var x in h.Seq do s := s + x; end; "
    "    end for; "
    "    Console.WriteLine($\"s={s}\"); "
    "    return 0; "
    "  end Main; "
    "end TMImpl; ";

// A holder whose `owned _f` has type `fieldTy`, built by `ctorBody`, with
// `members` spliced in before the property under test and `seq` being that
// property's whole declaration.
std::string holder(const char* fieldTy, const char* ctorBody,
                   const std::string& members, const std::string& seq) {
    return std::string("module TM; ") + kSelfA +
           "public class Holder "
           "  private owned _f: " + fieldTy + "; "
           "  private k: int := 1; "
           "  public constructor() " + ctorBody + " end constructor; " +
           members + seq + "end Holder; " + kMain;
}

// ARM 1 — the field and the property are the USER CLASS, so `for var x in
// h.Seq` selects `lowerIEnumerableFor`.
std::string userClassArm(const std::string& members, const std::string& get,
                         const char* virt = "") {
    return holder("SelfA", "_f := new SelfA(3);", members,
                  std::string("  public ") + virt + "property Seq: SelfA " +
                      get + " end Seq; ");
}

// ARM 2 — the field and the property are `IEnumerable<int>`, so the same read
// selects `lowerIEnumerableBuiltinFor`. The field already HAS the declared
// head, so §29.2 mints nothing at the read.
std::string ifaceArm(const std::string& members, const std::string& get) {
    return holder("IEnumerable<int>", "_f := new SelfA(3);", members,
                  "  public property Seq: IEnumerable<int> " + get +
                      " end Seq; ");
}

// CONTROL ARM — the field is an `Array<int>` and the property is
// `IEnumerable<int>`: SOMEWHERE on the way out §29.2 widens and mints a frame
// the reader owns.
std::string wideningArm(const std::string& members, const std::string& get) {
    return holder("Array<int>",
                  "var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); "
                  "_f := a;",
                  members,
                  "  public property Seq: IEnumerable<int> " + get +
                      " end Seq; ");
}

void expectCleanSum(const std::string& src, const char* sum) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // A use-after-free, a double free and a leaked frame all exit non-zero
    // under the checked oracle.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The loop genuinely ran 200 times: a row that never enumerated could not
    // print this.
    BEZEL_EXPECT_TRUE(r.stdoutText.find(sum) != std::string::npos);
}

const char* kBranchGetter =
    "get => do var t := _f; if k > 0 then return t; end; return t; end;";
const char* kElseGetter =
    "get => do var t := _f; if k > 5 then return _f; else return t; end; end;";
const char* kTryGetter =
    "get => do try var t := _f; return t; finally k := k + 1; end; end;";

}  // namespace

// ===========================================================================
// ROWS 1-4 — A `return` UNDER CONTROL FLOW. The laundering local is the one
// the first fix already followed; what it could not do is walk past the `if`.
// RED at the parent on both dispatch arms.
// ===========================================================================
BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           user_class_getter_return_under_branch_loop_must_not_reclaim_spec_28_9) {
    expectCleanSum(userClassArm("", kBranchGetter), "s=1200");
}

BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           ienumerable_getter_return_under_branch_loop_must_not_reclaim_spec_28_9) {
    expectCleanSum(ifaceArm("", kBranchGetter), "s=1200");
}

BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           user_class_getter_return_in_else_arm_loop_must_not_reclaim_spec_28_9) {
    expectCleanSum(userClassArm("", kElseGetter), "s=1200");
}

BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           ienumerable_getter_return_inside_try_loop_must_not_reclaim_spec_28_9) {
    expectCleanSum(ifaceArm("", kTryGetter), "s=1200");
}

// ===========================================================================
// ROWS 5-9 — THE HOP THROUGH ANOTHER PROPERTY, expression- and block-bodied,
// one hop and two. §28.9 makes the inner read a lend too, so the outer getter
// hands on storage the holder keeps. RED at the parent on both arms.
// ===========================================================================
BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           user_class_getter_through_property_loop_must_not_reclaim_spec_28_9) {
    expectCleanSum(
        userClassArm("  private property Inner: SelfA get => _f; end Inner; ",
                     "get => Inner;"),
        "s=1200");
}

BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           ienumerable_getter_through_property_loop_must_not_reclaim_spec_28_9) {
    expectCleanSum(
        ifaceArm("  private property Inner: IEnumerable<int> get => _f; "
                 "end Inner; ",
                 "get => Inner;"),
        "s=1200");
}

BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           user_class_block_getter_through_property_loop_must_not_reclaim_spec_28_9) {
    expectCleanSum(
        userClassArm("  private property Inner: SelfA get => _f; end Inner; ",
                     "get => do return Inner; end;"),
        "s=1200");
}

BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           ienumerable_block_getter_through_property_loop_must_not_reclaim_spec_28_9) {
    expectCleanSum(
        ifaceArm("  private property Inner: IEnumerable<int> get => _f; "
                 "end Inner; ",
                 "get => do var t := Inner; return t; end;"),
        "s=1200");
}

BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           user_class_getter_through_two_properties_loop_must_not_reclaim_spec_28_9) {
    expectCleanSum(
        userClassArm(
            "  private property Inner: SelfA get => _f; end Inner; "
            "  private property Mid: SelfA "
            "    get => do var m := Inner; return m; end; "
            "  end Mid; ",
            "get => Mid;"),
        "s=1200");
}

// ===========================================================================
// ROWS 10-11 — THE HOP THROUGH A METHOD. §28.3's announcement table, row 4:
// an instance method whose declared return is a plain concrete class "may
// return only a receiver-sourced borrow", so `Get()` hands back the holder's
// field and the getter hands it on. RED at the parent.
// ===========================================================================
BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           user_class_getter_through_borrow_returning_method_spec_28_3) {
    expectCleanSum(
        userClassArm("  private method Get(): SelfA return _f; end Get; ",
                     "get => Get();"),
        "s=1200");
}

BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           user_class_getter_local_bound_to_borrow_returning_method_spec_28_3) {
    expectCleanSum(
        userClassArm("  private method Get(): SelfA return _f; end Get; ",
                     "get => do var t := Get(); return t; end;"),
        "s=1200");
}

// ===========================================================================
// ROWS 12-13 — THE `virtual` POSITION, USER-CLASS-TYPED. The first fix
// declined every block-bodied getter on the vtable face, because §29.2's mint
// is not published there and `!minted` could not veto a claim. For a property
// whose type is a user-declared class there is no mint to publish — neither
// wrap creator fires for a non-iterator-format target, in this body or in any
// override — so the guard has nothing to protect. RED at the parent.
// (The `IEnumerable<int>`-typed virtual face stays declined: issue #6487.)
// ===========================================================================
BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           user_class_virtual_block_getter_loop_must_not_reclaim_spec_28_9) {
    expectCleanSum(
        userClassArm("", "get => do var t := _f; return t; end;", "virtual "),
        "s=1200");
}

BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           user_class_virtual_getter_through_property_loop_must_not_reclaim_spec_28_9) {
    expectCleanSum(
        userClassArm("  private property Inner: SelfA get => _f; end Inner; ",
                     "get => do return Inner; end;", "virtual "),
        "s=1200");
}

// ===========================================================================
// THE TWO BOUNDARY CONTROLS — THE MIRROR. Each MINTS a §29.2 frame on the way
// out, so the reader owns it and the loop must KEEP its reclamation. Green at
// the parent and green with the fix; each goes RED (6400 byte(s) leaked in
// 200 allocation(s)) under a plausible WRONG fix:
//   * the first under "a hop through a property is a lend" — a rule that
//     follows the hop without asking whether the inner read minted;
//   * the second under "trust the inner hop's verdict" — a rule that proves
//     the inner property lends and never checks the OUTER return edge.
// A 200-iteration leak count is not expressible as a conformance-corpus case.
// ===========================================================================

// The INNER property widens `Array<int>` to `IEnumerable<int>`: the frame is
// minted at the inner read, inside the outer getter.
BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           control_hop_through_widening_property_keeps_reclaim_spec_29_2) {
    expectCleanSum(
        wideningArm("  private property Inner: IEnumerable<int> get => _f; "
                    "end Inner; ",
                    "get => Inner;"),
        "s=1200");
}

// The inner property lends the `Array<int>` as it is; the OUTER return edge
// widens.
BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           control_outer_return_widens_a_lent_hop_keeps_reclaim_spec_29_2) {
    expectCleanSum(
        wideningArm("  private property Inner: Array<int> get => _f; "
                    "end Inner; ",
                    "get => do var t := Inner; return t; end;"),
        "s=1200");
}

// ===========================================================================
// fix-forward #6447 (review r42-rev-f20-6438, finding F1) — THE GENERIC
// OWNER. The property is declared `T` on `Holder<T>` and read through
// `Holder<SelfA>`, so the READ's type is the substituted `SelfA` while every
// type inside the getter's body is the unsubstituted `T`. Judging the
// getter's return edge `T` against the read's `SelfA` read "may mint" and
// refused the lend; the loop then reclaimed the holder's own `_f`. MEASURED
// on the reviewed tip (2e6744605 / a8705d794), `bezelc --sanitize=address`:
//
//   ERROR: AddressSanitizer: heap-use-after-free … in bz$Holder$DisposeCore$spec0
//
// where the parent (a44c7bec8) ran it clean, `s=1200`.
// ===========================================================================
namespace {

const char* kGenericHolder =
    "public class Holder<T> "
    "  private owned _f: T; "
    "  public constructor(owned f: T) _f := f; end constructor; "
    "  public property Seq: T get => do var t := _f; return t; end; end Seq; "
    "end Holder; ";

}  // namespace

// The review's own probe (g1_T_selfa_block_for.bzl, = q1). RED on the
// reviewed tip, green on the parent and with this fix.
BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           generic_holder_block_getter_launders_t_loop_must_not_reclaim_spec_28_9) {
    std::string src = std::string("module TM; ") + kSelfA + kGenericHolder +
        "public class TMImpl "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<200 do "
        "      var h := new Holder<SelfA>(new SelfA(3)); "
        "      for var x in h.Seq do s := s + x; end; "
        "    end for; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end TMImpl; ";
    expectCleanSum(src, "s=1200");
}

// The same generic getter reached through a HOP from a non-generic outer
// getter. The hop's return edge is judged inside `Holder<T>`'s body, so it
// needs the declared-type target as well; the one-hop floor does not follow
// a property hop at all. RED on the parent and on the reviewed tip.
BEZEL_TEST(for_in_getter_proven_lend_hops_6447,
           generic_holder_getter_through_outer_property_hop_must_not_reclaim_spec_28_9) {
    std::string src = std::string("module TM; ") + kSelfA + kGenericHolder +
        "public class Wrap "
        "  private owned _h: Holder<SelfA>; "
        "  public constructor() _h := new Holder<SelfA>(new SelfA(3)); "
        "  end constructor; "
        "  public property Seq: SelfA get => do var t := _h.Seq; return t; end; "
        "  end Seq; "
        "end Wrap; "
        "public class TMImpl "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<200 do "
        "      var w := new Wrap(); "
        "      for var x in w.Seq do s := s + x; end; "
        "    end for; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end TMImpl; ";
    expectCleanSum(src, "s=1200");
}
