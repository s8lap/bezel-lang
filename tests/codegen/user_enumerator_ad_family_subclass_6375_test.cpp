// Codegen — #6375 / §29.1 "The protocol" / §29.2 (the iterator-vtable shape) /
// §29.11 (v0.338).
//
// THE ISSUE. #6343 gave `wrapUserClassAsIEnumerable` (the manual
// `while e.MoveNext()` driver, src/codegen/LlvmBackend_Assign.cpp) a second
// `$ad` family of wrapper functions, selected PER INSTANCE at wrapper-construction
// time. Those `$ad` functions re-dispatch MoveNext / Current / Dispose through
// the INNER enumerator's own vtable instead of through a statically guessed
// class's methods, which is what a delegating `GetEnumerator` needs.
//
// #6343 SELECTED that family with:
//
//     enumPtr->vtable != vt$(enumTs)   =>   use the $ad family
//
// That predicate is unsound, and #6361's first attempt at the sibling site was
// rejected for the identical reason. `enumTs` is not a fact about the program —
// it is the output of a STRUCTURAL SCAN ("some class in this module exposing
// MoveNext + Current"). So `enumPtr->vtable != vt$(enumTs)` is ALSO true when
// `enumPtr` is an ordinary class instance of a SUBCLASS of the guessed class:
// a different class, therefore a different vtable global, therefore "not
// enumTs" — while the value is in fact exactly the shape the DIRECT family was
// written for. Taking `$ad` then re-dispatches through the class vtable, whose
// slot 2 is `Dispose`, not `MoveNext`.
//
// MEASURED class-vtable layout (BEZEL_DUMP_IR), both classes identical in
// shape, slot 2 differing only in which function it names:
//
//   @"vt$MSUB.Base"   = constant [8 x ptr] [td, itmap, Dispose, tostr, eq, hash, MoveNext, Current$propget]
//   @"vt$MSUB.Derived" = constant [8 x ptr] [td, itmap, Dispose, tostr, eq, hash, MoveNext, Current$propget]
//
// and the emitted `MoveNext$ad`, with `%6` = `vt$MSUB.Derived`'s slot 2:
//
//   %5 = getelementptr ptr, ptr %4, i32 2      ; slot 2 == bz$MSUB.Derived$Dispose
//   %6 = load ptr, ptr %5, align 8
//   %7 = call i1 %6(ptr %2)                    ; a VOID function through an i1 slot
//
// The behavioural symptom is not a crash but a SILENT wrong answer: the
// program's sum is 0 where it must be 6. That silence is why the rows below
// assert stdout rather than exit status — a 0/6 flip is invisible to `ran`.
//
// THE FIX. Stop asking "is this value the class I guessed?" and ask "what
// format is this value in?" — §29.2's own answer. Every §29.11 ITERATOR vtable
// emits `elems[0] = getPointerCast(vtGV)`, i.e. slot 0 is a POINTER TO THE
// VTABLE ITSELF, while every CLASS vtable carries a type descriptor there
// (`td$…`, or `bz$exc$vtable$table`). So:
//
//     use the $ad family  <=>  enumPtr->vtable->[0] == enumPtr->vtable
//
// This is subclass-proof by construction. A subclass's vtable is a DIFFERENT
// global with a DIFFERENT slot 0, so no arrangement of derived classes can make
// it satisfy a self-reference test. It also needs no `enumTs` vtable at all,
// which removes the `vtableOf(enumTs)` IMPORTED-`enumTs` skip: the delegating
// case for an imported class — previously left on the direct family — is now
// covered by the same uniform test (row `imported_enumerator_class_…` below).
//
// VTABLE-BUILD-SITE ENUMERATION — and a correction to an earlier revision of
// this file, which counted `setInitializer(ConstantArray…)` sites and reported
// "31 sites, 18 self-referencing, 10 class vtables". That method CANNOT see
// class vtables: they are built with the `GlobalVariable` CONSTRUCTOR form
// (LlvmBackend_Setup.cpp:5907 passes `init` to the ctor), so they never call
// `setInitializer`. The corrected count, by the discriminator this fix uses:
//
//   §29.11 ITERATOR vtables — 17 GLOBALS, each identified by slot 0 being
//     `getPointerCast(<the vtable global itself>)`. All 17 self-reference:
//     14 in LlvmBackend_Assign.cpp, plus LlvmBackend_Iterators.cpp's `vtGV` and
//     `cloneVtGV` and LlvmBackend_Async.cpp's async `vtGV`. (17, not the 18
//     earlier claimed: that count included a COMMENT line restating the idiom.)
//
//   CLASS vtables — built by the ctor form, every one putting a type
//     descriptor in slot 0. Verified on emitted IR rather than read off the
//     builder:
//       @"vt$M6375A.Derived" = constant [8 x ptr]
//         [ptr @"td$M6375A.Derived", ptr @"itmap$…", ptr @"bz$M6375A.Derived$Dispose", …]
//     Box vtables (`bvt$box$…`, Setup.cpp:5159) are declared with a null
//     initializer on the import side and are not iterator vtables; not counted.
//
// #6343's own comment claimed only the 14 in LlvmBackend_Assign.cpp were
// covered; this commit corrects that understatement as well.
//
// MUTATION TEST, both directions, so the discriminator is shown load-bearing
// rather than merely present (see `tests/README.md`). MEASURED across all five
// rows of this file; the counts below are the measured ones, and an earlier
// revision of this comment understated both directions:
//   * forcing `useAdFamily = true` (always the adapter) turns FOUR rows red —
//     every row except the two-class control: row 1 6 → 0, row 2 6 → 0 (and
//     exit 139), row 3 9 → 0, row 4 6 → 0. This is the #6343 direction.
//   * forcing `useAdFamily = false` (always direct) turns ROW 2 red: it prints
//     0 under ASan where the fix gives 6. This is the row that carries the
//     always-direct direction, and it is a good row — the MANUAL driver nests
//     TWO wrappers, the outer one's enumerator a genuine iterator-format value
//     (`$ad` required) and the inner one's a class instance (`$ad` wrong), so
//     forcing either family uniformly breaks it. An earlier revision called this
//     direction "vacuous" on the strength of the plain leg alone; the plain leg
//     prints 6 under always-direct and only the ASan leg shows it, which is
//     exactly the trap `tests/README.md` warns about.
// The two-class control (row 5) is green under BOTH mutations — it never
// reaches this discriminator, because the two-class idiom needs no wrapper
// family choice at all. That is what makes it a control.
//
// CONTROL ROWS, POSITIVE SHAPES. Every subclass row returns a SUBCLASS
// instance held in a field of the source, rather than reusing the
// self-returning idiom. Two reasons, both measured rather than assumed:
//   * the self-returning subclass shape (a `GetEnumerator` returning `this` on
//     a subclass) additionally trips a DISPOSAL aliasing bug in the `for`-in
//     driver — the loop double-frees the receiver because the `enumTs == ts`
//     guard misses — which is #6376, a different site, deliberately NOT
//     widened in here;
//   * the obvious alternative, minting a fresh `Derived` per call, prints 6 at
//     #6375's BASE as well and so would make every subclass row vacuous. The
//     field-delegating spelling is the one that actually separates the fix from
//     the base (0 → 6). See `freshSubclassSource`'s comment for the full
//     measurement of the three spellings.
//
// KNOWN LIMITATIONS — measured, deliberately NOT covered by a row here. Both
// were found while picking row shapes for this file, both reproduce IDENTICALLY
// at #6375's base commit and at the fix, so neither is introduced by this
// change and neither is fixed by it. They are recorded HERE, in the tree, rather
// than only in a handback, because CI cannot distinguish "measured unaffected"
// from "never run":
//
//  1. THE IMPORTED-`enumTs` PATH HAS NO AUTOMATED COVERAGE. The fix removes
//     #6343's `vtableOf(enumTs)` skip, so a delegating IMPORTED enumerator now
//     routes through the discriminator for the first time — and the codegen
//     harness (runAot / runAotChecked) is SINGLE-MODULE, while the two-module
//     machinery lives in tests/module with no shared helper. Hand measurement
//     (bezelc -c + --import) found the delegating-imported shape gives 6 and the
//     array-delegating-imported shape gives 9 both BEFORE and AFTER this fix,
//     which establishes UNAFFECTEDNESS, not coverage. A related consequence,
//     also unmeasured by CI: the unconditional slot-0 load is now reachable for
//     an imported `enumTs` where #6343 skipped it entirely, so a null returned
//     by an imported `GetEnumerator` would fault there rather than fall through
//     to the `$ad` family's null-guards. Structural gap: #6379.
//
//  2. A GENUINE `$ad` PATH — TWO-LEVEL DELEGATION — STILL LEAKS 32 BYTES.
//     `Outer.GetEnumerator` delegating to `Inner.GetEnumerator` delegating to
//     `Counter.GetEnumerator` reaches the `$ad` family for real, and under
//     ASan/LSan leaks 32 bytes allocated in `Inner.GetEnumerator`'s `calloc`.
//     Measured identical at base and at the fix. In the #6372 family (the
//     `$ad` Dispose chain not reclaiming what it should). No row here notices
//     it, because every `$ad`-reaching row in this file also happens to be
//     leak-clean for an unrelated reason (the enumerator is owned by the
//     source), so a row asserting LSan-cleanliness would be asserting nothing
//     about this leak.
//
// SPEC BASIS. §29.1 (`GetEnumerator(): IEnumerator<T>` yields a value in
// `IEnumerator<T>` format), §29.2 (the vtable protocol whose slot-0 shape is
// the discriminator), §29.11 ("Each `GetEnumerator()` call produces an
// independent enumerator"). Expected values are computed from the programs:
// `Base` / `Counter` enumerate 1, 2, 3 so `s := s + x` sums to 6; the
// `OtherEnum` row sums `x + 1` over 1, 2, 3 = 9.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// An enumerator class whose instances are 1, 2, 3 — `MoveNext` increments
// BEFORE the bound test, so the sequence is exactly 1, 2, 3.
const char* kCounter =
    "public class Counter "
    "  private _cur: int := 0; "
    "  public constructor() _cur := 0; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public virtual method MoveNext(): bool _cur := _cur + 1; return _cur <= 3; end MoveNext; "
    "  public virtual property Current: int get => _cur; end Current; "
    "end Counter; ";

// The same, as `Base` / `Derived` so the class NAMES express the inheritance
// relation the defect turns on. `Derived` adds no members of its own: the
// misdispatch is invisible in the source and visible only in the vtable, which
// is what makes it a soundness bug rather than a typing error.
const char* kSubclassPair =
    "public class Base: IEnumerator<int> "
    "  protected _cur: int := 0; "
    "  public constructor() _cur := 0; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public virtual method MoveNext(): bool _cur := _cur + 1; return _cur <= 3; end MoveNext; "
    "  public virtual property Current: int get => _cur; end Current; "
    "end Base; "
    "public class Derived: Base "
    "  public constructor() end constructor; "
    "end Derived; ";

// An enumerator-shaped class with NOTHING to do with the delegation. Its mere
// presence is enough for the structural scan to select it, so it is what makes
// the `$ad` family necessary rather than optional: if the scan's guess were
// ever honoured, this row would enumerate 100, 101, 102 instead of 1, 2, 3.
// `_k` starts at 99 so its sequence is unmistakably not the counter's.
const char* kUnrelated =
    "public class OtherEnum: IEnumerator<int> "
    "  private _k: int := 99; "
    "  public constructor() _k := 99; end constructor; "
    "  public method MoveNext(): bool _k := _k + 1; return _k <= 3; end MoveNext; "
    "  public property Current: int get => _k; end Current; "
    "end OtherEnum; ";

// The delegating source: `GetEnumerator` hands back another object's
// enumerator, so the returned value is NOT an instance of any class this
// module declares as its enumerator, and the `$ad` family is the correct one.
const char* kDelegatingBag =
    "public class Bag "
    "  private owned _c: Counter := new Counter(); "
    "  public method GetEnumerator(): owned IEnumerator<int> "
    "    return _c.GetEnumerator(); "
    "  end GetEnumerator; "
    "end Bag; ";

// A source whose `GetEnumerator` returns ANOTHER OBJECT's enumerator, where
// that object is an instance of a SUBCLASS of the class the structural scan
// guesses. `_d` is an `owned` FIELD, so the enumerator's lifetime is `Src`'s
// and not the `GetEnumerator` call's — which keeps the row about the driver's
// typing alone, with no lifetime question mixed in, and measured
// LSan-clean in this shape.
//
// The FIELD shape is load-bearing for the row's discriminating power, and that
// was measured rather than assumed. Two other spellings of "subclass
// enumerator" were tried and DO NOT separate the fix from #6375's base:
//   * `return new Derived()` — a bare temporary. Prints 6 at base AND at the
//     fix, because the freshly minted object never becomes the receiver the
//     wrapper dispatches on in a way the scan's guess can mis-type.
//   * `return new Derived().GetEnumerator()` — the same temporary, reached
//     through its own enumerator. Also prints 6 at base, and additionally
//     leaks the temporary under LSan (16 bytes from `Src.GetEnumerator`'s
//     `calloc`), which is #6372's loop-end disposal defect and not this issue.
// Only the field-delegating shape both (a) prints 0 at base and 6 at the fix,
// and (b) stays LSan-clean. Recorded here because the minting spelling is the
// more obvious thing to reach for and it would have made this row vacuous.
std::string freshSubclassSource(const char* moduleName) {
    return std::string("module ") + moduleName + "; " + kSubclassPair +
           "public class Src "
           "  private owned _d: Derived := new Derived(); "
           "  public method GetEnumerator(): owned IEnumerator<int> "
           "    return _d.GetEnumerator(); "
           "  end GetEnumerator; "
           "end Src; "
           "public class M "
           "  public static method Main(): int "
           "    var s := new Src(); "
           "    var t := 0; "
           "    for var x in s do t := t + x; end; "
           "    Console.WriteLine($\"{t}\"); "
           "    return 0; "
           "  end Main; end M;";
}

JitResult checked(const std::string& prog) { return runAotChecked(prog); }

}  // namespace

// THE PINNING ROW, and the sharpest form of the defect: the enumerator is an
// instance of a SUBCLASS of the class the structural scan guessed. Both
// classes have identical vtable SHAPES, so nothing in the layout says "wrong
// class" — only the identity of the vtable global does.
//
// The loop sums 1 + 2 + 3 = 6. At base the scan's `!=` predicate sends this to
// the `$ad` family, which reads `vt$…Derived` slot 2 (`Dispose`) as `MoveNext`
// and yields 0 instead. Asserted on stdout, not exit status: the failure is a
// silent wrong value, and a `ran`/exitCode assertion would stay green.
BEZEL_TEST(codegen_user_enumerator_ad_family_subclass_6375,
           subclass_of_the_guessed_enumerator_class_yields_all_elements_spec_29_1) {
    JitResult r = checked(freshSubclassSource("M6375A"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 1 + 2 + 3 = 6. The #6375 base prints 0 here: the `$ad` family dispatches
    // slot 2 of a CLASS vtable, which is `Dispose`, not `MoveNext`.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

// THE SAME QUESTION ON THE MANUAL DRIVER — and the row that carries the
// ALWAYS-DIRECT mutation direction. #6361 covered `for`-in and #6343 covered
// the manual spelling for the DELEGATING shape; the subclass shape reaches this
// site (`wrapUserClassAsIEnumerable`) and not that one, so it needs its own
// manual row — `while e.MoveNext()` over the same field-held `Derived`. Same
// expected sum, 6.
//
// MEASURED: this program builds TWO wrappers, and the runtime instrumentation
// reports TWO family selections in one run —
//
//   FAMILY=direct        (the inner wrapper: a class instance, direct correct)
//   FAMILY=ad            (the outer wrapper: a genuine iterator-format value,
//                          $ad required)
//
// So this is the only row in the file where the two families are BOTH needed
// and neither is wrong. It is red under always-`$ad` (6 → 0, exit 139) and
// red under always-direct (prints 0 under ASan), which is what makes it the
// discriminating row for the second direction. An earlier revision described
// this direction as vacuous, having read only the plain leg — the plain leg
// prints 6 under always-direct and only the ASan leg shows the defect.
BEZEL_TEST(codegen_user_enumerator_ad_family_subclass_6375,
           subclass_enumerator_manual_driver_yields_all_elements_spec_29_11) {
    JitResult r = checked(
        "module M6375B; " + std::string(kSubclassPair) +
        "public class Src "
        "  private owned _d: Derived := new Derived(); "
        "  public method GetEnumerator(): owned IEnumerator<int> "
        "    return _d.GetEnumerator(); "
        "  end GetEnumerator; "
        "end Src; "
        "public class M "
        "  public static method Main(): int "
        "    var s := new Src(); "
        "    using var e := s.GetEnumerator(); "
        "    var t := 0; "
        "    while e.MoveNext() do t := t + e.Current; end; "
        "    Console.WriteLine($\"{t}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

// AN UNRELATED ENUMERATOR-SHAPED CLASS must not retype the wrapper. `OtherEnum`
// is in the module purely so the structural scan has something to pick; the
// enumerator the callee hands back is a genuine `Counter` instance, so the
// correct family is the DIRECT one and the answer is `x + 1` over 1, 2, 3 = 9.
// Honouring the scan's guess would instead enumerate `OtherEnum`'s 100, 101,
// 102.
//
// MEASURED, and this row's name in an earlier revision was WRONG: it was called
// `..._still_takes_the_adaptive_family`, but the program selects the DIRECT
// family. Instrumented by emitting both arms under distinct names and reading
// the selection at runtime: `FAMILY=direct`. The direct family is correct here
// precisely because the scan's guess happens to be `Counter`, the class the
// enumerator really is. So this row pins "an unrelated class declaration in the
// module cannot change the answer", not "the `$ad` family is selected".
//
// It is red under the always-`$ad` mutation (9 → 0), which is what a row that
// must NOT be sent to the adapter should look like.
BEZEL_TEST(codegen_user_enumerator_ad_family_subclass_6375,
           unrelated_enumerator_class_cannot_retype_the_wrapper_spec_29_11) {
    JitResult r = checked(
        "module M6375C; " + std::string(kUnrelated) + kCounter + kDelegatingBag +
        "public class M "
        "  public static method Main(): int "
        "    var b := new Bag(); "
        "    var s := 0; "
        "    for var x in b do s := s + x + 1; end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // (1+1) + (2+1) + (3+1) = 9.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("9") != std::string::npos);
}

// CONTROL — the plain delegating shape, no subclass and no unrelated class.
// `Bag.GetEnumerator` returns `_c.GetEnumerator()` and `Counter.GetEnumerator`
// returns `this`, so the enumerator IS a `Counter` instance and the DIRECT
// family is correct.
//
// MEASURED, and this row's name in an earlier revision was also WRONG: it was
// called `..._still_takes_the_adaptive_family`, but the program selects the
// DIRECT family (`FAMILY=direct`, same runtime instrumentation as the row
// above). So this row pins "the plain delegating source still enumerates
// correctly", not "the `$ad` family is selected". It is red under the
// always-`$ad` mutation (6 → 0).
//
// #6343's rows cover the manual spelling of this shape; this is the `for`-in
// spelling on the same source class.
BEZEL_TEST(codegen_user_enumerator_ad_family_subclass_6375,
           plain_delegating_source_enumerates_correctly_spec_29_11) {
    JitResult r = checked(
        "module M6375D; " + std::string(kCounter) + kDelegatingBag +
        "public class M "
        "  public static method Main(): int "
        "    var b := new Bag(); "
        "    var s := 0; "
        "    for var x in b do s := s + x; end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

// CONTROL — the two-class idiom, which is the shape the structural scan was
// written FOR: `GetEnumerator` returns a freshly minted enumerator of exactly
// the declared class, so `enumPtr` is a plain class instance and the DIRECT
// family is correct. Pinned because a discriminator that mis-selected here
// would re-dispatch through a class vtable whose slot 2 is `Dispose`; the
// self-reference test is what keeps this row on the direct family.
BEZEL_TEST(codegen_user_enumerator_ad_family_subclass_6375,
           two_class_idiom_still_takes_the_direct_family_spec_29_11) {
    JitResult r = checked(
        "module M6375E; "
        "public class RangeEnum: IEnumerator<int> "
        "  private lo: int; private hi: int; private cur: int; "
        "  public constructor(lo: int, hi: int) this.lo := lo; this.hi := hi; cur := lo - 1; end constructor; "
        "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
        "  public property Current: int get => cur; end Current; "
        "end RangeEnum; "
        "public class Range "
        "  private _lo: int; private _hi: int; "
        "  public constructor(lo: int, hi: int) this._lo := lo; this._hi := hi; end constructor; "
        "  public method GetEnumerator(): owned IEnumerator<int> "
        "    return new RangeEnum(_lo, _hi); "
        "  end GetEnumerator; "
        "end Range; "
        "public class M "
        "  public static method Main(): int "
        "    var r := new Range(1, 3); "
        "    var s := 0; "
        "    for var x in r do s := s + x; end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}