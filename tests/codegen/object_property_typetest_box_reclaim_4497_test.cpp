// Codegen / §8.6 boxing — issue #4497: after #2760, a BLOCK-bodied `Object`
// getter's box still leaked at a TYPE-TEST read site — 24 B per read — while
// the EXPRESSION-bodied twin was clean. Measured on `ea93ea1c` with
// `scripts/agent_check.sh --probe --asan` over
// `for var i in 1..200 do using var h := new Holder(i); if h.O is int then … end;`:
// block-bodied `SUMMARY: AddressSanitizer: 4800 byte(s) leaked in 200
// allocation(s)`, expression-bodied clean, exit 7.
//
// Spec: §28.9 "Runtime-managed types", the *Accessor faces are uniform across
// type kinds* paragraph — "the two accessor body spellings of §23.3
// (`get => e` and `get => do … end`) are semantically indistinguishable at
// every read site", and "the implementation reclaims the production; how is
// not specified". §8.6 "Boxing" supplies the value: "Boxed values are
// runtime-managed (§28.9)". Spec revision v0.306.
//
// WHY THIS WAS A DEFECT AND NOT A RESIDUAL. Both spellings leaked at every
// read site before Family F, so the leak itself is not new. What #2760
// introduced is a DIVERGENCE: it wired `isUserPropertyBoxRead` into
// `boxLocalAssignmentSourceKind`, the classifier for a box BOUND to a local,
// and at a type-test site the expression-bodied getter is INLINED — its mint
// is visible through `lastBoxMint_`, so #2865's mint-identity disjunct already
// reclaimed it — while the block-bodied getter mints inside `bz$prop$O$get`
// and neither existing disjunct saw it. That made §28.9's "indistinguishable
// at every read site" false in the tree, which is the specific thing this
// project keeps paying for: a correct fix with a comment that over-claims its
// reach.
//
// THE FIX reuses #2760's predicate as a third disjunct of `lowerTypeTest`'s
// `freshBoxTemp` (LlvmBackend_Layout.cpp) rather than spelling "is this a
// fresh box" a third time — the property twin of the `isUserMethodBoxReturnCall`
// disjunct already there. Hoisting the WHOLE `boxLocalAssignmentSourceKind`
// instead is unsound and was measured so: it answers +1 for
// `isBoxableValueType(srcTy)`, which at a type test is the §12.7-NARROWED
// type, so a nested `o is T` inside `if o is int then` releases a box the
// local still owns — that variant aborts with `AddressSanitizer: attempting
// double-free`. Case (E) below is that shape, kept as a control.
//
// WARNING Every case is LOOP-AMPLIFIED (×200). A one-shot LSan row is not an
// oracle here: a single 24-byte box is routinely still reachable from a live
// stack slot or register at exit, so the row would pass on BOTH sides of the
// fix.
//
// MECHANICAL FAIL -> PASS (`--sanitize=address`, per case):
//   (A) 4800 B / 200 allocs -> clean   (block-bodied, TYPE-TEST site — the bug)
//   (B) clean before AND after         (expression-bodied, TYPE-TEST site)
//   (C) clean before AND after         (block-bodied, LOCAL-BIND site — #2760)
//   (D) clean before AND after         (expression-bodied, LOCAL-BIND site)
//   (E) clean before AND after         (nested §12.7 narrowing — the
//                                       double-free control for the rejected
//                                       broad variant)
//   (F) clean before AND after         (a getter that mints NO box: a release
//                                       there would be a UAF, not a leak)
// (A)–(D) together are the 2×2 grid the issue asks for: the §28.9 claim
// "indistinguishable at every read site" becomes executable rather than prose.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// The two §23.3 accessor body spellings of the SAME property, over the same
// `int` field. Everything else in the programs below is byte-identical, so a
// difference between an (A)/(C) row and its (B)/(D) twin is the body form and
// nothing else.
const char* kBlockGetter =
    "  public property O: Object "
    "    get => do "
    "      return _n; "
    "    end; "
    "  end O; ";
const char* kExprGetter =
    "  public property O: Object => _n; ";

std::string holder(const char* getter) {
    return std::string(
        "module M; "
        "class Holder "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; ") +
        getter +
        "end Holder; ";
}

// The TYPE-TEST read site: the property read is an UNBOUND temporary, the
// operand of `is`. §12.7 — the test "inspects the runtime type tag without
// moving from the operand", so the box is dead the moment the test answers.
std::string typeTestSite(const char* getter) {
    return holder(getter) +
        "class P "
        "  public static method Main(): int "
        "    var hits := 0; "
        "    for var i in 1..200 do "
        "      using var h := new Holder(i); "
        "      if h.O is int then hits := hits + 1; end; "
        "    end; "
        "    if hits = 200 then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end P;";
}

// The LOCAL-BIND read site — what #2760 closed. Kept here so the grid is
// complete and so a future change to either face fails the other's row too.
std::string localBindSite(const char* getter) {
    return holder(getter) +
        "class P "
        "  public static method Main(): int "
        "    var hits := 0; "
        "    for var i in 1..200 do "
        "      using var h := new Holder(i); "
        "      var o := h.O; "
        "      if o is int then hits := hits + 1; end; "
        "    end; "
        "    if hits = 200 then return 7; end; "
        "    return 1; "
        "  end Main; "
        "end P;";
}

// =====================================================================
// (E) CONTROL — nested §12.7 narrowing. `o` is a BOUND local whose box the
// local-bind path already owns and releases at scope end; the inner test's
// operand is the bare identifier `o`, which #3090 deliberately lowers as its
// BOX rather than its narrowed value. Releasing there would be a double
// free, and that is not hypothetical: the rejected broad variant of this fix
// (`freshBoxTemp` = `boxLocalAssignmentSourceKind(...) == 1`) was built and
// aborts on this program with `AddressSanitizer: attempting double-free`.
// The shipped disjunct declines because an identifier is not a
// `MemberAccessExpr`. §12.7 / §8.6 / §28.9.
// =====================================================================
const char* kNestedNarrowing =
    "module M; "
    "interface IThing "
    "  method Tag(): int; "
    "end IThing; "
    "class Holder "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public property O: Object "
    "    get => do "
    "      return _n; "
    "    end; "
    "  end O; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var hits := 0; "
    "    for var i in 1..200 do "
    "      using var h := new Holder(i); "
    "      var o: Object := h.O; "
    "      if o is int then "
    "        if o is IThing then hits := hits + 1000; end; "
    "        hits := hits + 1; "
    "      end; "
    "    end; "
    "    if hits = 200 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

// =====================================================================
// (F) CONTROL — `Object`-typed getters that mint NO box, read at the TYPE-TEST
// site the fix widened. §8.6 boxes only a VALUE type at an `Object` location;
// a class reference and a `string` keep their own representation. Classifying
// either as a +1 producer would hand a class instance or a `%bz.string` to
// `bz_box_release`, whose rc word lands at offset 8 — inside the instance's
// disposed-flag word. Clean under the strict oracle: nothing leaked, and
// nothing may be freed.
// =====================================================================
const char* kNonBoxingGettersAtTypeTest =
    "module M; "
    "class Thing "
    "  public N: int := 0; "
    "  public constructor(n: int) N := n; end constructor; "
    "end Thing; "
    "class Holder "
    "  private _t: Thing; "
    "  public constructor(t: Thing) _t := t; end constructor; "
    "  public property AsObj: Object => _t; "
    "  public property AsStr: Object => \"hi\"; "
    "end Holder; "
    "class P "
    "  public static method Main(): int "
    "    var acc := 0; "
    "    for var i in 1..200 do "
    "      using var t := new Thing(i); "
    "      using var h := new Holder(t); "
    "      if h.AsObj is Thing then acc := acc + 1; end; "
    "      if h.AsStr is string then acc := acc + 1; end; "
    "    end; "
    "    if acc = 400 then return 7; end; "
    "    return 1; "
    "  end Main; "
    "end P;";

}  // namespace

// ---------------------------------------------------------------------
// (A) the issue's own repro — block-bodied getter at the TYPE-TEST site.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_property_typetest_box_reclaim_4497,
           block_bodied_getter_typetest_box_is_reclaimed_spec_28_9) {
    JitResult r = runAotChecked(typeTestSite(kBlockGetter));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);          // pre-fix: 4800 B / 200 allocations
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_property_typetest_box_reclaim_4497,
           block_bodied_getter_typetest_plain_value_invariant_spec_12_7) {
    // NOT a #4497 oracle — a leak is invisible without a sanitizer, so this
    // passes with and without the fix. It pins that the fix did not change
    // the answer, i.e. that the box is not freed before `is` reads its
    // descriptor.
    JitResult r = runAot(typeTestSite(kBlockGetter));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (B) the expression-bodied twin at the same site — already clean via
// #2865's mint identity, and it must stay clean (not double-released now
// that a second disjunct also answers true for it).
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_property_typetest_box_reclaim_4497,
           expr_bodied_getter_typetest_box_is_reclaimed_spec_28_9) {
    JitResult r = runAotChecked(typeTestSite(kExprGetter));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_property_typetest_box_reclaim_4497,
           expr_bodied_getter_typetest_plain_value_invariant_spec_23_3) {
    JitResult r = runAot(typeTestSite(kExprGetter));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (C)/(D) the LOCAL-BIND site, both spellings — #2760's face, pinned here
// so the 2x2 grid is executable.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_property_typetest_box_reclaim_4497,
           block_bodied_getter_localbind_box_is_reclaimed_spec_28_9) {
    JitResult r = runAotChecked(localBindSite(kBlockGetter));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_property_typetest_box_reclaim_4497,
           expr_bodied_getter_localbind_box_is_reclaimed_spec_28_9) {
    JitResult r = runAotChecked(localBindSite(kExprGetter));
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (E) CONTROL: nested §12.7 narrowing must not double-free.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_property_typetest_box_reclaim_4497,
           nested_narrowed_identifier_is_not_released_spec_12_7) {
    JitResult r = runAotChecked(kNestedNarrowing);
    if (wasSkipped(r)) return;
    // The rejected broad variant aborts here with
    // "AddressSanitizer: attempting double-free" (measured).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_property_typetest_box_reclaim_4497,
           nested_narrowed_identifier_plain_value_invariant_spec_12_7) {
    JitResult r = runAot(kNestedNarrowing);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (F) CONTROL: getters that mint no box must not be released.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_object_property_typetest_box_reclaim_4497,
           non_boxing_getters_at_typetest_are_not_released_spec_8_6) {
    JitResult r = runAotChecked(kNonBoxingGettersAtTypeTest);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_object_property_typetest_box_reclaim_4497,
           non_boxing_getters_at_typetest_plain_invariant_spec_8_6) {
    JitResult r = runAot(kNonBoxingGettersAtTypeTest);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
