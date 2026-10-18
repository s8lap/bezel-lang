// Codegen — #6376 / §28.9 (disposal) / §29.1 / §29.11 (v0.338).
//
// THE ISSUE. `for var x in d do … end` where `d`'s class INHERITS the
// self-returning enumerator idiom from a base — `GetEnumerator()` returning
// `this`, declared on the BASE — double-frees the receiver. Correct
// enumeration, then a hard SEGV at exit.
//
// MEASURED, on the repro in the first row, at the commit that fixed #6375:
//
//   plain leg : selfsub=6   exit 139
//   ASan leg  : selfsub=6, then
//               ERROR: AddressSanitizer: heap-use-after-free
//               READ of size 8 ... in main
//               0x… is located 0 bytes inside of 24-byte region
//               freed by thread T0 here: ... in main
//               previously allocated by thread T0 here: ... calloc ... in main
//
// The three frames are all `main`, which is the whole diagnosis: the free and
// the read are two of the compiler's OWN reclamation sites in the same
// function. The 24-byte region is the enumerator.
//
// ROOT CAUSE. `lowerIEnumerableFor` (src/codegen/LlvmBackend_Stmts.cpp) knows
// the loop's enumerator might BE the source object — the self-returning idiom,
// where `enumPtr == srcPtr` — and guards the loop's disposal accordingly:
//
//     if (enumTs == ts && sourceIsNamedOwner) { …identity-guard to null… }
//     …
//     if (!sourceIsNamedOwner && enumTs != ts && …) { …register source… }
//
// `enumTs` is not a fact about the program. It is the output of a STRUCTURAL
// SCAN (LlvmBackend_Stmts.cpp:10163-10227, "some class in this module
// exposing MoveNext + Current"). For `SelfDerived` the scan finds `SelfBase` —
// the class that actually DECLARES `GetEnumerator`/`MoveNext`/`Current` — so
// `enumTs == SelfBase` while `ts == SelfDerived`, and the identity comparison
// is FALSE even though the enumerator IS the source object.
//
// Both guards then take the wrong branch. The aliasing guard does not fire, so
// the loop reclaims the receiver; and the source's own scope reclaims it again.
// Confirmed in the IR: `free(%36)` where `%36` is loaded from the ENUMERATOR
// slot, and `free(%89)` where the value came from the SOURCE slot, both in
// `main`, on the same 24-byte block. At the fix the aliasing guard is emitted
// (`ien.selfen` / `ien.enown` — grep count 0 before, 3 after).
//
// THE FIX. The question the guard asks is a question about the VALUE, so it
// must be asked of the value's type relationship: "is `enumTs` `ts` or a CLASS
// ANCESTOR of `ts`?". Walking `ts`'s class base chain covers the inherited
// self-returning idiom, which is the shape that actually occurs — a class
// implementing `IEnumerable<T>` for its consumers inherits it; it does not
// redeclare it. The second site is widened in lockstep, because it must be the
// negation of the first or the two disagree and one of them reclaims a shared
// object.
//
// This is a strictly WIDENING OF A SAFETY GUARD, and it is deliberately not
// more than that. The runtime `enumPtr == srcPtr` compare (#2384's argument,
// unchanged) still decides whether the two pointers actually alias, so a
// self-implementing class that constructs and returns a FRESH instance of its
// own type keeps the ordinary distinct-object disposal. Row
// `self_implementing_class_returning_a_fresh_instance_…` below pins that.
//
// SAME ROOT CAUSE, DIFFERENT SITE. #6375 fixed the identical mis-guess in
// `wrapUserClassAsIEnumerable` (`enumPtr->vtable != vt$(enumTs)` selecting the
// `$ad` family). That is a different function and a different question —
// dispatch, not disposal — so it is a separate commit. The two defects share
// only the premise that the scan's `enumTs` is not the type of the value.
//
// NOT WIDENED IN. The loop-end disposal still drives the enumerator through the
// §28.2 class convention (the value's own vtable slot 2), which on an
// iterator-vtable-format value is `MoveNext`, not `Dispose` — #6372, a
// separate lane.
//
// SPEC BASIS. §28.9 (every heap object is disposed exactly once), §29.1
// (`GetEnumerator(): IEnumerator<T>`), §29.11 ("Each `GetEnumerator()` call
// produces an independent enumerator"). Expected values are computed from the
// programs: `SelfBase.MoveNext` increments before the bound test, so `hi = 3`
// yields exactly 1, 2, 3 and the sum is 6.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The self-returning enumerator, declared on the BASE. `SelfDerived` below
// inherits `GetEnumerator`, `MoveNext` and `Current` without redeclaring any of
// them, which is what makes the structural scan pick `SelfBase`.
const char* kSelfPair =
    "public class SelfBase: IEnumerable<int>, IEnumerator<int> "
    "  protected cur: int := 0; protected hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public virtual method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public virtual property Current: int get => cur; end Current; "
    "end SelfBase; "
    "public class SelfDerived: SelfBase "
    "  public constructor(hi: int) base(hi); end constructor; "
    "end SelfDerived; ";

// A self-implementing class that DECLARES the protocol itself and returns a
// FRESH instance of its own type from `GetEnumerator` — the case #2384's
// runtime `enumPtr == srcPtr` compare exists for. It must keep the ordinary
// distinct-object disposal: two objects, two disposals, no aliasing.
const char* kFreshSelf =
    "public class FreshSelf: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return new FreshSelf(hi); end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end FreshSelf; ";

JitResult checked(const std::string& prog) { return runAotChecked(prog); }

}  // namespace

// THE PINNING ROW: a NAMED LOCAL of the subclass type, iterated with `for`-in.
// At base this prints the correct `selfsub=6` and then SEGVs (plain exit 139;
// ASan heap-use-after-free), so the row fails on `r.ran` / `exitCode` before
// the fix and passes after. stdout carries the value so it is asserted without
// relying on an 8-bit exit-code truncation — and here the value is CORRECT
// even at base, which is exactly why this bug hid: only the exit-time
// reclamation faults.
BEZEL_TEST(codegen_forin_selfenumerator_subclass_double_free_6376,
           inherited_self_returning_enumerator_named_local_spec_29_11) {
    JitResult r = checked(
        "module M6376A; " + std::string(kSelfPair) +
        "public class M "
        "  public static method Main(): int "
        "    var d := new SelfDerived(3); "
        "    var s := 0; "
        "    for var x in d do s := s + x; end; "
        "    Console.WriteLine($\"selfsub={s}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("selfsub=6") != std::string::npos);
}

// THE SAME SHAPE UNDER AN EXPLICIT `using` BOUND. `using var` disposes at the
// end of its own scope rather than at the function's, so the two reclamation
// sites are not the same ones as the row above — but the aliasing question is
// identical, and the double free is the same defect. Distinct program, distinct
// disposal sites, same guard.
BEZEL_TEST(codegen_forin_selfenumerator_subclass_double_free_6376,
           inherited_self_returning_enumerator_using_var_spec_29_11) {
    JitResult r = checked(
        "module M6376B; " + std::string(kSelfPair) +
        "public class M "
        "  public static method Main(): int "
        "    var s := 0; "
        "    using var d := new SelfDerived(3); "
        "    for var x in d do s := s + x; end; "
        "    Console.WriteLine($\"selfsub={s}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("selfsub=6") != std::string::npos);
}

// THE BARE TEMPORARY, `for var x in new SelfDerived(3)`. Here `sourceIsNamedOwner`
// is FALSE — the `new` result is bound to no name anywhere — so the FIRST guard
// does not apply and this exercises the SECOND site (the separate source
// registration) instead. At base this crashes BEFORE printing anything, via
// that registration rather than via the loop-end reclaim, so the row asserts
// the value as well as survival.
BEZEL_TEST(codegen_forin_selfenumerator_subclass_double_free_6376,
           inherited_self_returning_enumerator_bare_temporary_spec_29_11) {
    JitResult r = checked(
        "module M6376C; " + std::string(kSelfPair) +
        "public class M "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in new SelfDerived(3) do s := s + x; end; "
        "    Console.WriteLine($\"selfsub={s}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("selfsub=6") != std::string::npos);
}

// THE CONTROL THAT KEEPS THE WIDENING HONEST — #2384's case. `FreshSelf`
// DECLARES the enumerator protocol and its `GetEnumerator` returns a FRESH
// instance rather than `this`, so `enumIsTsOrBase` is true but `enumPtr !=
// srcPtr` at runtime. The widened guard must then leave the ordinary
// distinct-object disposal in place: two distinct objects, each disposed once.
// If the widening had replaced the runtime compare with the static fact, this
// program would free an object the loop does not own and the ASan row would
// fail — so the row is what distinguishes "widened the safety guard" from
// "trusted a structural fact".
//
// The source is a NAMED LOCAL, which is the spelling that exercises the guard
// at all: `sourceIsNamedOwner` is what the first site is gated on. The
// bare-temporary spelling of this same class was tried first and rejected for
// this row because it leaks 24 bytes under LSan — measured PRE-EXISTING, at
// #6376's base commit as well as at the fix, so it is not this issue's
// defect and folding it in would have made the row fail for an unrelated
// reason. The named spelling is clean at base and at the fix.
BEZEL_TEST(codegen_forin_selfenumerator_subclass_double_free_6376,
           self_implementing_class_returning_a_fresh_instance_is_not_aliased_6376) {
    JitResult r = checked(
        "module M6376D; " + std::string(kFreshSelf) +
        "public class M "
        "  public static method Main(): int "
        "    var s := 0; "
        "    var d := new FreshSelf(3); "
        "    for var x in d do s := s + x; end; "
        "    Console.WriteLine($\"fresh={s}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("fresh=6") != std::string::npos);
}

// CONTROL — the UNINHERITED self-returning idiom, which the guard already
// handled before this fix (`enumTs == ts` held). Pinned so the ancestor walk
// cannot have changed what the exact test did: same value, same single
// disposal, LSan-clean.
BEZEL_TEST(codegen_forin_selfenumerator_subclass_double_free_6376,
           direct_self_returning_enumerator_still_disposes_once_spec_29_11) {
    JitResult r = checked(
        "module M6376E; "
        "public class DirectSelf: IEnumerable<int>, IEnumerator<int> "
        "  private cur: int := 0; private hi: int; "
        "  public constructor(hi: int) this.hi := hi; end constructor; "
        "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
        "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
        "  public property Current: int get => cur; end Current; "
        "end DirectSelf; "
        "public class M "
        "  public static method Main(): int "
        "    var d := new DirectSelf(3); "
        "    var s := 0; "
        "    for var x in d do s := s + x; end; "
        "    Console.WriteLine($\"direct={s}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("direct=6") != std::string::npos);
}

// CONTROL — the TWO-CLASS idiom over a SUBCLASS source. `RangeDerived.GetEnumerator`
// returns a fresh `RangeEnum`, so the enumerator is a genuinely distinct object
// and the source must still be registered and disposed separately. This is the
// `!enumIsTsOrBase` branch of the second site, and it is where an over-widened
// guard would leak: skipping the source registration here would leave `d`
// undisposed. Pinned under ASan so a leak is caught rather than tolerated.
BEZEL_TEST(codegen_forin_selfenumerator_subclass_double_free_6376,
           two_class_idiom_over_a_subclass_still_disposes_the_source_6376) {
    JitResult r = checked(
        "module M6376F; "
        "public class RangeEnum: IEnumerator<int> "
        "  private lo: int; private hi: int; private cur: int; "
        "  public constructor(lo: int, hi: int) this.lo := lo; this.hi := hi; cur := lo - 1; end constructor; "
        "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
        "  public property Current: int get => cur; end Current; "
        "end RangeEnum; "
        "public class RangeBase "
        "  protected _lo: int; protected _hi: int; "
        "  public constructor(lo: int, hi: int) this._lo := lo; this._hi := hi; end constructor; "
        "  public method GetEnumerator(): owned IEnumerator<int> return new RangeEnum(_lo, _hi); end GetEnumerator; "
        "end RangeBase; "
        "public class RangeDerived: RangeBase "
        "  public constructor(lo: int, hi: int) base(lo, hi); end constructor; "
        "end RangeDerived; "
        "public class M "
        "  public static method Main(): int "
        "    var d := new RangeDerived(1, 3); "
        "    var s := 0; "
        "    for var x in d do s := s + x; end; "
        "    Console.WriteLine($\"two={s}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("two=6") != std::string::npos);
}