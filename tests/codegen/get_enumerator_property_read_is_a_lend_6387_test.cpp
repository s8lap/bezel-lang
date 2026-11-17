// Codegen — #6387 / §28.9 + §28.2 + §29.11 (v0.357). The explicit
// `.GetEnumerator()` consumer over a computed `IEnumerable<T>` property read.
//
// THE RESIDUAL. #6387 ("a wrapper over a computed IEnumerable<T> property
// FREES a field the holder owns") was closed for the wrap producer and for
// the two for-in consumers (#6397, #6447). A THIRD consumer still carried the
// classification those fixes retired. `h.Seq.GetEnumerator()` on a
// vtable-backed receiver dispatches the clone slot (#2376) and then asks
// whether the RECEIVER is a temporary it must dispose — and for a property
// read it asked `isAutoPropertyStorageSymbol`, reading "not an auto-property"
// as "a fresh value the receiver expression owns". So a getter that hands back
// the holder's own field had that field disposed at the enclosing scope's end,
// and the holder's teardown then read freed memory.
//
// MEASURED at this fix's parent, `bezelc --sanitize=address`, on every row
// below marked RED — and with no `using` on the enumerator at all, so the
// free is the RECEIVER's, not the enumerator's:
//
//   ==NNNN==ERROR: AddressSanitizer: heap-use-after-free on address 0x…
//       #0 … in bz$TM.Holder$DisposeCore
//       #1 … in bz$TM.Holder$Dispose
//       #2 … in main
//   freed by thread T0 here:
//       #1 … in bz$arrenumown$i32$Dispose
//   previously allocated by thread T0 here:
//       #1 … in bz$TM.Holder$ctor
//
// The same programs over the FIELD (`h.F.GetEnumerator()`) or over a local
// bound to the read (`var e := h.Seq; e.GetEnumerator()`) are clean at the
// parent: the defect is this consumer's verdict on the property read.
//
// THE RULE. §28.9: "A property or indexer read is a lend for every declared
// type … a getter never transfers a reclamation obligation to its reader".
// §28.2 keeps the holder's release point the only place its field is
// reclaimed. §29.11: "Each `GetEnumerator()` call produces an independent
// enumerator" — so the enumerator is the caller's and the source stays the
// holder's. The consumer now asks the one rule the for-in consumers ask,
// `Emitter::propertyReadIsLend`: `isAuto || (lend && !minted)`.
//
// THE MIRROR. Where the read itself MINTS a §29.2 frame (the getter widens an
// `Array<int>` or a user class to `IEnumerable<int>`), that frame is the
// reader's and this consumer must KEEP disposing it; a lend claim there is a
// leak. The two control rows pin both widening shapes.

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

const char* kArrayCtor =
    "var a := new Array<int>(); a.Add(1); a.Add(2); a.Add(3); _f := a;";
const char* kSelfACtor = "_f := new SelfA(3);";

// The manual protocol, one enumerator.
const char* kDrive =
    "using var it := h.Seq.GetEnumerator(); "
    "while it.MoveNext() do s := s + it.Current; end; ";

// 200 iterations, the holder constructed and torn down inside the loop: the
// receiver's wrong free and the holder's teardown read are 200 distinct
// events, and a leaked frame is 200 allocations.
std::string prog(const char* fieldTy, const char* ctorBody,
                 const std::string& members, const std::string& get,
                 const char* body) {
    return std::string("module TM; ") + kSelfA +
           "public class Holder "
           "  private owned _f: " + fieldTy + "; "
           "  public constructor() " + ctorBody + " end constructor; " +
           members +
           "  public property Seq: IEnumerable<int> " + get + " end Seq; "
           "end Holder; "
           "public class TMImpl "
           "  public static method Main(): int "
           "    var s := 0; "
           "    for var i in 0..<200 do "
           "      var h := new Holder(); " + body +
           "    end for; "
           "    Console.WriteLine($\"s={s}\"); "
           "    return 0; "
           "  end Main; "
           "end TMImpl; ";
}

void expectCleanSum(const std::string& src, const char* sum) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    // A use-after-free, a double free and a leaked frame all exit non-zero
    // under the checked oracle.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The enumerator was really driven 200 times.
    BEZEL_EXPECT_TRUE(r.stdoutText.find(sum) != std::string::npos);
}

}  // namespace

// ===========================================================================
// ROWS 1-2 — THE ISSUE'S SHAPE: `property Seq: IEnumerable<int> get => _f`
// over an `owned` field that already HAS the declared head, so §29.2 mints
// nothing at the read. Row 1's field holds an array-backed frame, row 2's a
// user class. RED at the parent.
// ===========================================================================
BEZEL_TEST(get_enumerator_property_read_is_a_lend_6387,
           array_backed_field_getter_receiver_is_not_disposed_spec_28_9) {
    expectCleanSum(
        prog("IEnumerable<int>", kArrayCtor, "", "get => _f;", kDrive),
        "s=1200");
}

BEZEL_TEST(get_enumerator_property_read_is_a_lend_6387,
           user_class_backed_field_getter_receiver_is_not_disposed_spec_28_9) {
    expectCleanSum(
        prog("IEnumerable<int>", kSelfACtor, "", "get => _f;", kDrive),
        "s=1200");
}

// ===========================================================================
// ROWS 3-4 — THE OTHER GETTER SPELLINGS. §28.9: the two accessor body
// spellings "are semantically indistinguishable at every read site", and the
// lend survives any number of hops (#6447). RED at the parent.
// ===========================================================================
BEZEL_TEST(get_enumerator_property_read_is_a_lend_6387,
           block_getter_through_local_receiver_is_not_disposed_spec_28_9) {
    expectCleanSum(
        prog("IEnumerable<int>", kArrayCtor, "",
             "get => do var t := _f; return t; end;", kDrive),
        "s=1200");
}

BEZEL_TEST(get_enumerator_property_read_is_a_lend_6387,
           getter_through_property_receiver_is_not_disposed_spec_28_9) {
    expectCleanSum(
        prog("IEnumerable<int>", kArrayCtor,
             "  private property Inner: IEnumerable<int> get => _f; "
             "end Inner; ",
             "get => Inner;", kDrive),
        "s=1200");
}

// ===========================================================================
// ROW 5 — §29.11: "Each `GetEnumerator()` call produces an independent
// enumerator". Two enumerators over the same property read, each driven to
// the end: both see 1, 2, 3 and the holder still owns its field afterwards.
// RED at the parent (the first call's receiver disposal frees the field the
// second call then reads).
// ===========================================================================
BEZEL_TEST(get_enumerator_property_read_is_a_lend_6387,
           two_enumerators_over_one_property_are_independent_spec_29_11) {
    expectCleanSum(
        prog("IEnumerable<int>", kArrayCtor, "", "get => _f;",
             "using var a := h.Seq.GetEnumerator(); "
             "using var b := h.Seq.GetEnumerator(); "
             "while a.MoveNext() do s := s + a.Current; end; "
             "while b.MoveNext() do s := s + b.Current; end; "),
        "s=2400");
}

// ===========================================================================
// THE TWO BOUNDARY CONTROLS — THE MIRROR. The getter WIDENS to
// `IEnumerable<int>`, so §29.2 mints a frame at the read; nobody but this
// consumer holds it, and the receiver disposal must stay. Green at the parent
// and green with the fix; both go RED (200 leaked frames) under the plausible
// WRONG fix "a property receiver is always named" — the one-line change that
// closes rows 1-5 without asking whether the read minted. The two rows are
// the two wrap creators (array-backed, user-class-backed). A 200-iteration
// leak count is not expressible as a conformance-corpus case.
// ===========================================================================
BEZEL_TEST(get_enumerator_property_read_is_a_lend_6387,
           control_array_widening_getter_keeps_receiver_disposal_spec_29_2) {
    expectCleanSum(
        prog("Array<int>", kArrayCtor, "", "get => _f;", kDrive), "s=1200");
}

BEZEL_TEST(get_enumerator_property_read_is_a_lend_6387,
           control_user_class_widening_getter_keeps_receiver_disposal_spec_29_2) {
    expectCleanSum(
        prog("SelfA", kSelfACtor, "", "get => _f;", kDrive), "s=1200");
}
