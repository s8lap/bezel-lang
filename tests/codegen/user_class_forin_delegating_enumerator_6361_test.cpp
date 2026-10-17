// Codegen — #6361 / §29.1 "The protocol" / §29.11 (v0.338).
//
// THE ISSUE. `for var x in b do … end` where `b` is a class whose
// `GetEnumerator()` DELEGATES (`return _c.GetEnumerator();`) **SEGVs** in
// `bz$Counter$MoveNext`. Measured identical at base and at the tip carrying
// #6343's fix, so pre-existing — found by reviewer `r31-rv-6343` while
// reviewing #6343 and filed separately from it.
//
// ROOT CAUSE — the same TYPE CONFUSION #6343 repaired, reached through a
// DIFFERENT driver. Both `wrapUserClassAsIEnumerable` (LlvmBackend_Assign.cpp,
// the manual `while e.MoveNext()` path) and `lowerIEnumerableFor`
// (LlvmBackend_Stmts.cpp, THIS path) pick the concrete enumerator class
// `enumTs` by the same STRUCTURAL SCAN — a class exposing MoveNext + Current.
// That guess is right for the two-class idiom (`return new IntRangeEnum(...)`)
// and for the self-returning one (`return this`), and wrong when the source's
// `GetEnumerator` DELEGATES.
//
// #6343 gave the wrapper a second `$ad` family selected by a runtime identity
// test (`enumPtr->vtable == vt$(enumTs)`) that re-dispatches through the
// value's own iterator slots. **The `for`-in driver never ran that test.** It
// reproduced the same guess and then drove the result with the guessed class's
// own `MoveNext` / `Current` — through NO vtable at all. MEASURED by IR diff
// (see the two IR quotes in this file's `useOwnVtable` comment): both drivers
// emit the identical `%call i1 @…MoveNext` / `getelementptr %class.Counter`
// pair; the only difference is that the manual driver's receiver is a real
// `Counter` while the `for`-in driver's is the `userenumenum` wrapper struct.
//
// So the delegating shape SEGVs. `Counter` is `{ptr vtable, i8 disposed,
// i32 _cur}` — `_cur` at byte 12 — and the wrapper is `{ptr vtable, ptr
// enumerator, ptr source, i1 ownsSrc}`, whose `enumerator` POINTER occupies
// bytes 8..15. Reading `_cur` at byte 12 therefore reads **bits 32..47 of a
// heap address**. ASan, at base:
//
//   ERROR: AddressSanitizer: SEGV on unknown address 0x7bb652de001c
//   #0 bz$P9.Counter$MoveNext
//   rdi = 0x00007bb652de001c   <- the wrapper's enumerator pointer + 0xc
//
// THE FIX. `enumTs` is a guess and the guess cannot be trusted statically, so
// the same runtime identity test #6343 uses decides the family: when the
// enumerator's slot-0 vtable is not `enumTs`'s own class vtable, the value is
// already in §29.11 iterator-vtable format and MoveNext / Current must be read
// through ITS OWN slots 2 / 3. The direct path is kept in its own basic block
// so the two-class and self-returning idioms keep byte-identical generated
// code (and so `emitMethodCall`'s class-virtual handling and null fallback are
// untouched on the path that still uses them).
//
// Slot 0 is a BARE LOAD of the object, not a `GEP vtable, 0; load`. A class
// vtable's `[0]` is its type descriptor, so the extra deref compares
// `td$…` against `vt$…`, never matches, and silently sends the two-class and
// self-returning idioms down the adapter path — where slot 3 of a CLASS vtable
// is `tostr`, not `Current`. That regression was measured (both controls fell
// from 6 to 0) and is what the two CONTROL rows below now pin.
//
// NOT COVERED HERE, DELIBERATELY. Two neighbouring holes are NOT this issue
// and are not widened into it:
//   * an IMPORTED `enumTs` — `vtableOf` returns null (imports have no local
//     vtable global, LlvmBackend_Setup.cpp ~L5096), so the test is skipped and
//     the pre-existing direct lowering stands. Filed separately.
//   * the loop's DISPOSAL still drives the wrapper through the §28.2 CLASS
//     dispose convention (the object's own vtable slot 2), which on an
//     iterator-vtable-format value is `MoveNext`, not `Dispose`. Benign where
//     the delegated enumerator is owned by the source (every row here, measured
//     LSan-clean); repairing it needs a new `Disposal` kind in
//     LlvmBackend_Disposal.cpp. Filed separately.
//
// SPEC BASIS. §29.1 (`GetEnumerator(): IEnumerator<T>` yields an
// `IEnumerator<T>`-FORMAT value), §29.11 ("Each `GetEnumerator()` call produces
// an independent enumerator"), §29.2 (the iterator-vtable protocol:
// [1]=Dispose, [2]=MoveNext, [3]=Current, [4]=GetEnumerator). Expected values
// are computed from the programs: `Counter` enumerates 1, 2, 3, so
// `s := s + x` sums to 6; the repeat row runs 201 fresh enumerations for 1206.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A class implementing the enumerator protocol by the #2384 self-returning
// idiom: `GetEnumerator()` returns `this`.
const char* kCounter =
    "public class Counter "
    "  private _cur: int := 0; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public method MoveNext(): bool _cur := _cur + 1; return _cur <= 3; end MoveNext; "
    "  public property Current: int get => _cur; end Current; "
    "end Counter; ";

// The DELEGATING source. `_c` is an `owned` FIELD, so the Counter's lifetime
// is Bag's rather than the GetEnumerator method's — which keeps every row here
// about the driver's typing alone, with no lifetime question mixed in.
const char* kDelegatingBag =
    "public class Bag "
    "  private owned _c: Counter := new Counter(); "
    "  public method GetEnumerator(): owned IEnumerator<int> "
    "    return _c.GetEnumerator(); "
    "  end GetEnumerator; "
    "end Bag; ";

// The consuming driver, shared by the delegating rows. `for var x in b` is the
// driver under test — #6343 already covers the manual `while e.MoveNext()`
// spelling over the same class.
std::string delegatingForIn(const char* moduleName, const char* extras = "") {
    return std::string("module ") + moduleName + "; " + extras + kCounter +
           kDelegatingBag +
           "public class M "
           "  public static method Main(): int "
           "    var b := new Bag(); "
           "    var s := 0; "
           "    for var x in b do s := s + x; end; "
           "    Console.WriteLine($\"{s}\"); "
           "    return 0; "
           "  end Main; end M;";
}

JitResult checked(const std::string& prog) { return runAotChecked(prog); }

}  // namespace

// THE PINNING ROW. `for var x in b` over a delegating Bag. At base this is a
// hard SEGV in `bz$Counter$MoveNext` on both oracles, so the row fails on
// `r.ran` / `exitCode` before the fix and passes after. stdout carries the
// value, so it is asserted without relying on an 8-bit exit-code truncation.
BEZEL_TEST(codegen_user_class_forin_delegating_enumerator_6361,
           delegating_getenumerator_forin_yields_all_elements_spec_29_11) {
    JitResult r = checked(delegatingForIn("M6361A"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

// The same program on the PLAIN leg. Companion, not the proof — it exists so a
// future change cannot make the plain oracle disagree with the checked one
// again, which is the failure mode this whole family is about.
BEZEL_TEST(codegen_user_class_forin_delegating_enumerator_6361,
           delegating_getenumerator_forin_plain_leg_agrees_spec_29_11) {
    JitResult r = runAot(delegatingForIn("M6361B"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

// THE SHARPEST FORM. An enumerator-shaped class with nothing to do with the
// delegation is declared in the module — all it takes for the structural scan
// to select it as `Bag`'s enumerator class and drive ITS methods over `_c`'s
// adapter. `OtherEnum` enumerates 100, 101, 102, so honouring the scan's guess
// would print 303 rather than 6. At base: SEGV in `bz$C2.Counter$MoveNext`.
BEZEL_TEST(codegen_user_class_forin_delegating_enumerator_6361,
           unrelated_enumerator_class_cannot_retype_the_forin_driver_6361) {
    const char* kOther =
        "public class OtherEnum: IEnumerator<int> "
        "  private _k: int := 99; "
        "  public constructor() _k := 99; end constructor; "
        "  public method MoveNext(): bool _k := _k + 1; return _k <= 3; end MoveNext; "
        "  public property Current: int get => _k; end Current; "
        "end OtherEnum; ";
    JitResult r = checked(delegatingForIn("M6361C", kOther));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

// THE OTHER DELEGATION TARGET: a builtin collection. `Bag` holds the array in a
// plain (non-`owned`) field fed from a constructor parameter rather than in an
// `owned` field initialised from a literal, because an `Array` FIELD's backing
// store is never reclaimed (#6341, filed separately — it reproduces with no
// enumerator in the program). This is an ASan row, so that leak would
// otherwise be attributed here; measured LSan-clean in this shape.
BEZEL_TEST(codegen_user_class_forin_delegating_enumerator_6361,
           array_delegating_getenumerator_forin_spec_29_11) {
    const char* kOther =
        "public class OtherEnum: IEnumerator<int> "
        "  private _k: int := 99; "
        "  public constructor() _k := 99; end constructor; "
        "  public method MoveNext(): bool _k := _k + 1; return _k <= 3; end MoveNext; "
        "  public property Current: int get => _k; end Current; "
        "end OtherEnum; ";
    JitResult r = checked(
        std::string("module M6361D; ") + kOther +
        "public class Bag "
        "  private _a: Array<int>; "
        "  public constructor(a: Array<int>) _a := a; end constructor; "
        "  public method GetEnumerator(): owned IEnumerator<int> "
        "    return _a.GetEnumerator(); "
        "  end GetEnumerator; "
        "end Bag; "
        "public class M "
        "  public static method Main(): int "
        "    var a: Array<int> := [1, 2, 3]; "
        "    var b := new Bag(a); "
        "    var s := 0; "
        "    for var x in b do s := s + x; end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

// NOT A ONE-SHOT REPRO. The defect is per-call, so the loop drives 201
// independent delegating enumerations; each one is a fresh `userenumenum`
// wrapper with its own identity test. 201 * (1 + 2 + 3) = 1206. Measured
// LSan-clean, so the adapter path does not leak a wrapper per iteration.
BEZEL_TEST(codegen_user_class_forin_delegating_enumerator_6361,
           repeated_delegating_forin_never_leaks_or_drifts_6361) {
    JitResult r = checked(
        std::string("module M6361E; ") + kCounter + kDelegatingBag +
        "public class M "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0 .. 200 do "
        "      var b := new Bag(); "
        "      for var x in b do total := total + x; end; "
        "    end; "
        "    Console.WriteLine($\"{total}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1206") != std::string::npos);
}

// CONTROL — the DIRECT family is untouched. The two-class idiom is the shape
// the structural scan was written FOR: `IntRange.GetEnumerator()` really does
// `return new IntRangeEnum(...)`, so the enumerator's slot-0 vtable IS
// `vt$IntRangeEnum`, the identity test selects the original lowering, and the
// loop sums 1 + 2 + 3 = 6. This row is the one that caught the slot-0
// double-dereference during development: with it, the class vtable's `[0]`
// (`td$…`) was compared against `vt$…`, never matched, and this row fell to 0.
BEZEL_TEST(codegen_user_class_forin_delegating_enumerator_6361,
           two_class_idiom_forin_still_takes_the_direct_family_spec_29_11) {
    JitResult r = checked(
        "module M6361F; "
        "class IntRangeEnum: IEnumerator<int> "
        "  private lo: int; private hi: int; private cur: int; "
        "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; cur:=lo-1; end constructor; "
        "  public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext; "
        "  public property Current: int get => cur; end Current; "
        "end IntRangeEnum; "
        "class IntRange: IEnumerable<int> "
        "  private lo: int; private hi: int; "
        "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; end constructor; "
        "  public method GetEnumerator(): IEnumerator<int> return new IntRangeEnum(lo, hi); end GetEnumerator; "
        "end IntRange; "
        "public class M "
        "  public static method Main(): int "
        "    using var r := new IntRange(1, 3); "
        "    var s := 0; for var x in r do s := s + x; end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

// CONTROL — the SELF-RETURNING idiom is untouched. `SelfIter` implements BOTH
// interfaces and returns `this`, so `structurallyIsEnumerator(srcTs)` is true,
// `enumTs == srcTs`, and the enumerator's slot-0 vtable IS `vt$SelfIter` — the
// identity test selects the direct family here too. Also measured at 6 before
// the fix; it is the second of the two controls the slot-0 double-dereference
// regression moved to 0.
BEZEL_TEST(codegen_user_class_forin_delegating_enumerator_6361,
           self_returning_idiom_forin_still_takes_the_direct_family_spec_29_11) {
    JitResult r = checked(
        "module M6361G; "
        "public class SelfIter: IEnumerable<int>, IEnumerator<int> "
        "  private cur: int := 0; private hi: int; "
        "  public constructor(hi: int) this.hi := hi; end constructor; "
        "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
        "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
        "  public property Current: int get => cur; end Current; "
        "end SelfIter; "
        "public class M "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in new SelfIter(3) do s := s + x; end; "
        "    Console.WriteLine($\"{s}\"); "
        "    return 0; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}