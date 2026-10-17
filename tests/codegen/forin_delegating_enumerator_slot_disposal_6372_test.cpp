// Codegen — #6372 / §28.2 + §29.2 + §29.11 (v0.338).
//
// THE ISSUE. `for var x in b do … end` where `b`'s `GetEnumerator()`
// DELEGATES to another object's (`return _r.GetEnumerator();`) leaks the
// enumerator the loop MINTS. Found — measured, not fixed — by lane
// `r31-p-codegen-forin` while fixing #6361; filed as #6372 because the repair
// needs a new `Disposal` kind, outside that lane's territory.
//
// THE DEFECT. Loop-end disposal applied the §28.2 CLASS convention — call the
// object's own vtable slot 2 — but on a value in §29.11 ITERATOR-vtable format
// slot 2 is `MoveNext`, not `Dispose`. The wrapper was therefore freed without
// ever having its `Dispose` called, so the enumerator it held was never
// disposed.
//
// MEASURED on the fix's parent (r31-c1 @ 7201a5bc), `bezelc
// --sanitize=address` on the one-shot repro:
//
//   6
//   ==ERROR: LeakSanitizer: detected memory leaks
//   Direct leak of 24 byte(s) in 1 object(s) allocated from:
//     #1 bz$D1.Rng$GetEnumerator
//
// Read off the IR, not from the source. The loop's `ien.end` reached
// `using.cls.dispose35`, which loads the wrapper's vtable, GEPs slot 2 and
// calls it — and the wrapper's table is
//
//   @"bz$userenumenum$Rng$RngEnum$vtable" = [ vtable, $Dispose, $MoveNext,
//                                             $Current, $GetEnumerator ]
//
// so the call at loop end was `MoveNext`, and the 32-byte wrapper itself WAS
// reclaimed by the block release that follows — which is exactly why the leak
// is the 24-byte `RngEnum` inside it rather than the wrapper.
//
// The CONTROL is the identical program driven manually (`using var e :=
// b.GetEnumerator(); while e.MoveNext() do … end`): also prints 6, and
// LSan-CLEAN. #6343's `$ad` family gives that driver the iterator vtable's
// real Dispose slot, so the loop-end slot-2 convention never sees the
// delegating shape there.
//
// WHY THE STATIC TYPE CANNOT ANSWER IT. `lowerIEnumerableFor` picks `enumTs`
// by a STRUCTURAL scan (a class exposing MoveNext + Current). That guess is
// right for the two-class idiom (`return new RngEnum(lo, hi)`) and for the
// self-returning one (`return this`), and wrong for the delegating one — the
// value is not an `enumTs` instance at all.
//
// THE FIX. `iterEnumOrClassDispose`, a new `Disposal` kind: at disposal time
// the value is asked which family it is in, using §29.2's own POSITIVE shape
// test — the iterator vtable's slot-0 SELF-REFERENCE. Every §29.11 table this
// compiler emits puts itself in slot 0 (`vtable[0] == vtable`); every CLASS
// vtable puts a type descriptor there, including a SUBCLASS's, which is a
// different global with a different [0]. It is therefore the same test
// `lowerIEnumerableFor` already uses to drive MoveNext / Current (`useOwnVtable`,
// fix #6361), so the three sites cannot disagree about the family. On the
// iterator arm, slot 1 and no block release (that Dispose frees its own frame);
// on the class arm, the ordinary §28.2 `emitDisposeAndReclaim`, unchanged.
//
// NOT #6361's REJECTED TEST. #6361's first attempt used "the receiver's vtable
// is not `vt$(enumTs)`" — a NEGATIVE predicate that a subclass satisfies by
// being a subclass (`6ac6fabf` was REJECTED for exactly that; `for var x in
// new SelfDerived(3)` printed 6 at base and 0 with it). Only a positive test
// can exclude inheritance. The subclass discriminator itself is pinned by
// #6361's own `subclass_of_enumerator_takes_the_direct_family_spec_29_11` row,
// which is green on both sides of this change — so it is deliberately NOT
// duplicated here.
//
// DELIBERATELY NOT COVERED: a SUBCLASS enumerator reached through a
// SELF-RETURNING delegate (a `Derived` instance that `GetEnumerator` returns as
// `this`). That shape is a SEPARATE, PRE-EXISTING defect in
// `wrapUserClassAsIEnumerable`'s `$ad`-family SELECTION
// (LlvmBackend_Assign.cpp), which picks the `$ad` family by the same rejected
// negative identity test against `vt$(Base)` — so a subclass instance is sent
// to a `$ad` wrapper whose `Dispose$ad` then calls slot 1 of a CLASS vtable,
// i.e. `itmap`, and wild-jumps. Measured: that shape PRINTS 0 at the fix's
// parent (already wrong, silently), and this fix makes the wrongness LOUD
// rather than fixing it. It is #6375's, not #6372's — a live claim on exactly
// that function — so no row here can be green on both sides and pinning it
// here would misattribute the defect.
//
// SPEC BASIS. §29.1 (a `GetEnumerator(): IEnumerator<T>` yields an
// `IEnumerator<T>`-FORMAT value), §29.11 ("Each `GetEnumerator()` call produces
// an independent enumerator"), §29.2 (the iterator-vtable protocol,
// [1]=Dispose [2]=MoveNext [3]=Current [4]=GetEnumerator), §28.2 (the class
// dispose convention, vtable slot 2). Expected values are computed from the
// programs: `Rng` enumerates 1, 2, 3, so one pass sums to 6 and 301 passes sum
// to 1806; `TwoClass` enumerates 10, 20, 30 and `SelfBase` 1, 2, 3.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The enumerator class, in the plain two-class idiom.
const char* kRngEnum =
    "public class RngEnum: IEnumerator<int> "
    "  private lo: int; private hi: int; private cur: int; "
    "  public constructor(lo: int, hi: int) this.lo := lo; this.hi := hi; "
    "    cur := lo - 1; end constructor; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; "
    "  end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end RngEnum; ";

// The inner collection, whose GetEnumerator MINTS a fresh `RngEnum` on every
// call — this is what makes the delegating shape leak at all. A self-returning
// or source-owned delegate has nothing minted for the loop to own.
const char* kRng =
    "public class Rng: IEnumerable<int> "
    "  private lo: int; private hi: int; "
    "  public constructor(lo: int, hi: int) this.lo := lo; this.hi := hi; "
    "  end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> "
    "    return new RngEnum(lo, hi); "
    "  end GetEnumerator; "
    "end Rng; ";

// The DELEGATING source. `_r` is an `owned` field, so the Rng's lifetime is
// Bag's and the only thing the loop owns is what `GetEnumerator` minted.
const char* kDelegatingBag =
    "public class Bag "
    "  private owned _r: Rng; "
    "  public constructor() _r := new Rng(1, 3); end constructor; "
    "  public method GetEnumerator(): owned IEnumerator<int> "
    "    return _r.GetEnumerator(); "
    "  end GetEnumerator; "
    "end Bag; ";

// The loop-amplified delegating driver. 301 passes x (1+2+3) = 1806, returned
// as `acc mod 100` = 6. The amplification is the point: the defect is 24 bytes
// PER PASS, so a one-shot LSan row would sit inside LSan's own conservative-scan
// jitter and could pass on both sides.
std::string amplifiedDelegatingForIn(const char* moduleName) {
    return std::string("module ") + moduleName + "; " + kRngEnum + kRng +
           kDelegatingBag +
           "public class M "
           "  public static method Main(): int "
           "    var acc := 0; "
           "    for var i in 0 .. 300 do "
           "      var b := new Bag(); "
           "      var s := 0; "
           "      for var x in b do s := s + x; end; "
           "      acc := acc + s; "
           "    end; "
           "    Console.WriteLine($\"{acc}\"); "
           "    return acc mod 100; "
           "  end Main; end M;";
}

// The one-shot delegating driver, printing 6.
const char* kDelegatingOnce =
    "public class M "
    "  public static method Main(): int "
    "    var b := new Bag(); "
    "    var s := 0; "
    "    for var x in b do s := s + x; end; "
    "    Console.WriteLine($\"{s}\"); "
    "    return 0; "
    "  end Main; end M;";

// The issue's CONTROL: the byte-identical classes driven MANUALLY. #6343's
// `$ad` family already gives this path the iterator vtable's real Dispose slot,
// so it is LSan-clean at the fix's parent. It pins that the loop-end convention
// was specific to the `for`-in driver, not to the delegation itself.
std::string manualDelegatingFor(const char* moduleName) {
    return std::string("module ") + moduleName + "; " + kRngEnum + kRng +
           kDelegatingBag +
           "public class M "
           "  public static method Main(): int "
           "    var b := new Bag(); "
           "    var s := 0; "
           "    using var e := b.GetEnumerator(); "
           "    while e.MoveNext() do s := s + e.Current; end; "
           "    Console.WriteLine($\"{s}\"); "
           "    return 0; "
           "  end Main; end M;";
}


JitResult checked(const std::string& prog) { return runAotChecked(prog); }

// The amplified drivers return `acc mod 100` (a body-preservation device, so
// the loop cannot be constant-folded away). Both residues below are < 256, so
// the exit code is asserted WITHOUT relying on 8-bit truncation — and stdout
// carries the untruncated value, which is what the value assertions read.
constexpr int kResidueOf1806 = 6;    // 1806 mod 100
constexpr int kResidueOf301 = 1;     //  301 mod 100

}  // namespace

// ===========================================================================
// THE PINNING ROW. Loop-amplified (301x) delegating `for`-in over a source
// whose GetEnumerator mints a fresh enumerator per call.
//
// At the fix's parent: ASan reports `Direct leak of 24 byte(s) in 1 object(s)`
// per pass — 7,224 B over 301 passes — and the run exits nonzero, so the row
// fails on `exitCode`. With the fix: no sanitizer finding, residue 6.
//
// ORACLE. `runAotChecked` is ALWAYS ASan and yields a nonzero exit on ANY
// sanitizer finding, so a leak shows up as a failed RUN, not as a silently
// missing finding. The leak-oracle and the ASan-oracle therefore agree here,
// which is not true of every leak row in this codebase.
// ===========================================================================
BEZEL_TEST(forin_delegating_enumerator_slot_disposal_6372,
           amplified_delegating_forin_reclaims_the_minted_enumerator_spec_29_11) {
    JitResult r = checked(amplifiedDelegatingForIn("M6372A"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf1806);
    // 301 x 6 = 1806 — the loop genuinely ran, so a constant-folded body
    // cannot make this row vacuously clean.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1806") != std::string::npos);
}

// The one-shot form. Not the leak oracle (the row above is 301x); it pins the
// VALUE, so a fix that disposed the right SLOT of the wrong OBJECT would still
// be caught here.
BEZEL_TEST(forin_delegating_enumerator_slot_disposal_6372,
           one_shot_delegating_forin_still_yields_every_element_spec_29_11) {
    JitResult r = checked(std::string("module M6372B; ") + kRngEnum + kRng +
                          kDelegatingBag + kDelegatingOnce);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

// THE CONTROL THAT WAS ALREADY CLEAN. The byte-identical classes driven
// MANUALLY rather than by `for`-in. Green BOTH before and after the fix —
// which is the point: the defect was the loop-end convention, not the
// delegation, and a "fix" that regressed the manual path would show here.
// MEASURED on the fix's parent: LSan-clean, prints 6.
BEZEL_TEST(forin_delegating_enumerator_slot_disposal_6372,
           manual_delegating_driver_agrees_forin_spec_29_11) {
    JitResult r = checked(manualDelegatingFor("M6372C"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("6") != std::string::npos);
}

// THE CLASS ARM STILL RUNS. A NON-delegating `for`-in over `Rng`, whose
// GetEnumerator returns `new RngEnum(lo, hi)`, yields a genuine class instance
// — so the §28.2 convention (slot 2 + block release) is the correct one and
// must still be taken. Had the new arm mis-selected the family it would call
// slot 1 of a class vtable, which is `itmap` — a wild jump. `acc` = 1806.
BEZEL_TEST(forin_delegating_enumerator_slot_disposal_6372,
           two_class_idiom_forin_still_uses_the_class_dispose_convention_spec_28_2) {
    JitResult r = checked(
        std::string("module M6372D; ") + kRngEnum + kRng +
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var i in 0 .. 300 do "
        "      var r := new Rng(1, 3); "
        "      var s := 0; "
        "      for var x in r do s := s + x; end; "
        "      acc := acc + s; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf1806);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1806") != std::string::npos);
}

// THE SELF-RETURNING IDIOM. `GetEnumerator` returns `this`, so the loop's
// enumerator IS the source; the identity-guarded `enumDisposeSlot` nulls it
// (the source is a named owner) and the disposal is SKIPPED. This is the row
// that would catch an arm which ran the slot-1 Dispose on a value that was
// never minted for the loop. `acc` = 1806.
BEZEL_TEST(forin_delegating_enumerator_slot_disposal_6372,
           self_returning_getenumerator_forin_still_agrees_spec_29_11) {
    JitResult r = checked(
        std::string("module M6372E; ") +
        "public class SelfBase: IEnumerable<int> "
        "  private cur: int := 0; "
        "  public method GetEnumerator(): IEnumerator<int> return this; "
        "  end GetEnumerator; "
        "  public method MoveNext(): bool cur := cur + 1; return cur <= 3; "
        "  end MoveNext; "
        "  public property Current: int get => cur; end Current; "
        "end SelfBase; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var i in 0 .. 300 do "
        "      var b := new SelfBase(); "
        "      var s := 0; "
        "      for var x in b do s := s + x; end; "
        "      acc := acc + s; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf1806);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1806") != std::string::npos);
}

// A BREAK out of the delegating loop reaches the same endBB disposal — the
// `disposeStack_` scope is popped by the break path — so it must be leak-free
// too, not only the normal-exhaustion path. `acc` counts one break per pass,
// so 301, residue 1. This row is the one that pins the DISPOSAL SCOPE rather
// than the slot: a fix that only reclaimed on `MoveNext() == false` would pass
// every other row here and leak this one.
BEZEL_TEST(forin_delegating_enumerator_slot_disposal_6372,
           delegating_forin_break_reaches_the_same_disposal_spec_29_11) {
    JitResult r = checked(
        std::string("module M6372F; ") + kRngEnum + kRng + kDelegatingBag +
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var i in 0 .. 300 do "
        "      var b := new Bag(); "
        "      for var x in b do acc := acc + 1; break; end; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf301);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("301") != std::string::npos);
}
