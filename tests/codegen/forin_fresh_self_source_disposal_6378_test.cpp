// Codegen — #6378 / §28.3 + §28.9 + §29.1 + §29.11 (v0.338).
//
// THE ISSUE (a). `for var x in new FreshSelf(3) do … end`, where
// `FreshSelf` implements the enumerator interface ITSELF and its
// `GetEnumerator()` returns a FRESH instance of its OWN type
// (`return new FreshSelf(hi);`), leaks 24 bytes per pass. Filed by lane
// `r31-p-codegen-forin` after #6372 landed; a SIBLING of #6372, not of #6375.
//
// THE DEFECT. `lowerIEnumerableFor` (src/codegen/LlvmBackend_Stmts.cpp) has two
// disposal registrations for the loop it owns — the enumerator's, and (since
// #2424) the unowned temporary SOURCE's — and the source's was gated on
// `!enumIsTsOrBase`:
//
//     if (!sourceIsNamedOwner && !enumIsTsOrBase && disposerFns_.count(ts)) {
//         …register the source…
//     }
//
// `enumIsTsOrBase` is not a fact about the program. It is a fact about two
// SYMBOLS the structural enumerator scan picked ("does `enumTs` equal `ts`, or
// a class ancestor of it" — the condition #6376 widened). It conflates "the
// enumerator's static type is related to the source's" with "the enumerator
// IS the source object", and for this shape the two are DIFFERENT OBJECTS:
// `GetEnumerator` mints a second `FreshSelf`. The gate therefore suppressed
// the only disposal the unowned temporary source would ever get, and the
// source leaked — silently, with no wrong-vtable-slot, no wrong answer, and a
// correct enumeration printed first.
//
// THIS IS #2384'S OWN ARGUMENT, ALREADY WRITTEN DOWN AND NOT IMPLEMENTED.
// The comment at the `enumPtr == srcPtr` compare says the guard's static
// condition "is only a STRUCTURAL fact (the class exposes MoveNext + Current,
// and is the source's own type or a base of it), not a proof that this
// particular `GetEnumerator()` body returns `this` — a self-implementing
// class could instead construct and return a fresh instance of its own type,
// in which case the two pointers differ and the ordinary distinct-object
// disposal is correct and must not be skipped". #6376's own test file records
// the consequence it could not keep: the `FreshSelf` row there uses a NAMED
// local because "the bare-temporary spelling of this same class was tried
// first and rejected for this row because it leaks 24 bytes under LSan".
//
// MEASURED, at this fix's parent (r30-c1 @ 95c1e290), `bezelc
// --sanitize=address`, 301 passes of `for var x in new FreshSelf(1, 3)`:
//
//   1806
//   Direct leak of 7224 byte(s) in 301 object(s)
//   SUMMARY: AddressSanitizer: 7224 byte(s) leaked in 301 allocation(s)
//
// 7224 / 301 = 24 B per pass — exactly one `FreshSelf`. A LEAK, not a
// use-after-free: the summary line is LeakSanitizer's, and nothing in the run
// reports a UAF, a double free, or a wild jump. Contrast #6372/#6376, which
// are loud; this one is silent, which is why it survived three lanes.
//
// WHICH OBJECT LEAKS — the issue says the ENUMERATOR; it is the SOURCE. Read
// off the IR, not inferred: in `main` the `for`-in head's `new FreshSelf` is
// `%12`, `bz$M.FreshSelf$GetEnumerator` does its OWN `calloc` yielding `%36`,
// and the loop-end disposal reaches only `%5` (the enumerator slot). The
// enumerator IS reclaimed, through `ien.cls.dispose`; the source is not
// reclaimed at all. Confirmed by CONTROL row `fresh_self_named_source_is_…`
// below: binding the same object to a name is LSan-CLEAN, because a named
// source is disposed at its own scope end and never needed this registration.
//
// THE FIX. Make the runtime compare the only authority, asked in the direction
// the OWNERSHIP requires. Before, `enumPtr == srcPtr` was evaluated only when
// `sourceIsNamedOwner && enumIsTsOrBase`; the source's own registration was
// gated by the static `enumIsTsOrBase` instead. Now:
//   * NAMED source — the source is disposed at its OWN scope end, so the loop
//     must not reclaim it: the ENUMERATOR entry is suppressed when they alias
//     (`ien.selfen` / `ien.enown`, unchanged — #2384's and #6376's guard).
//   * UNNAMED source — the loop is its SOLE owner, so the SOURCE entry is
//     suppressed when they alias (`ien.srcisenum` / `ien.srconly`), and fires
//     when they do not. That is the defect: two distinct objects, two
//     disposals.
// In both directions exactly one disposal survives the aliasing case, so the
// self-returning idiom still disposes once, and a genuinely distinct source is
// now disposed as well. The suppression is conditional on the enumerator's
// entry actually being registered — with no enumerator disposer there is no
// entry to do the disposal, and nulling the source would trade a leak for a
// bigger one.
//
// NOT THE OTHER HALF OF #6378. Its finding (b) is a two-module
// delegating-imported `for`-in that prints the right answer then SEGVs at exit.
// That needs `bezelc -c` + `--import`, which the codegen AOT harness cannot
// express — it is the single-module structural gap #6379, and it is NOT
// covered here. Nothing in this file claims it.
//
// NOT #6372's DISCRIMINATOR. #6372's slot-0 self-reference (`vtable[0] ==
// vtable`) is a THIRD transcription of the family test and is filed as #6381;
// this defect is not in it — the enumerator here is an ordinary class
// instance and always took the class arm, correctly, on both sides of this
// change. The only transcriptions touched are the two aliasing guards in this
// one function.
//
// SPEC BASIS. §28.3 (ownership of a produced value), §28.9 (every heap object
// is disposed exactly once), §29.1 (`GetEnumerator(): IEnumerator<T>`),
// §29.11 ("Each `GetEnumerator()` call produces an independent enumerator" —
// which is precisely why a fresh instance of the source's own type is a
// SECOND object that needs its own disposal). Expected values are computed
// from the programs: `MoveNext` increments before the bound test, so `hi = 3`
// yields exactly 1, 2, 3, one pass sums to 6 and 301 passes sum to 1806
// (1806 mod 100 = 6, under 256 so the exit code carries it without relying on
// 8-bit truncation).

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// THE DEFECTIVE CLASS. It DECLARES the enumerator protocol itself (so the
// structural scan picks it as `enumTs`, and `enumTs == ts`), and its
// `GetEnumerator` returns a FRESH instance of its own type rather than `this`.
const char* kFreshSelf =
    "public class FreshSelf: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> "
    "    return new FreshSelf(hi); "
    "  end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end FreshSelf; ";

// A SOURCE TYPE THAT INHERITS `GetEnumerator` without redeclaring it, so the
// structural scan picks the BASE (`FreshSelf`) while `ts` is the derived class
// — i.e. `enumIsTsOrBase` holds only through #6376's ancestor walk, and the
// two pointers still differ. This is the shape that would have leaked before
// #6376 existed and leaks again now.
const char* kFreshDerived =
    "public class FreshDerived: FreshSelf "
    "  public constructor(hi: int) base(hi); end constructor; "
    "end FreshDerived; ";

// The self-returning pair: `GetEnumerator` returns `this`. Green on BOTH sides
// of this change — it is what the aliasing compare must PRESERVE.
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

// The two-class idiom and the delegating idiom — `enumIsTsOrBase` is FALSE for
// both, so the source registration fired before this change and must still
// fire (now behind a runtime compare that evaluates false).
const char* kTwoClass =
    "public class RngEnum: IEnumerator<int> "
    "  private lo: int; private hi: int; private cur: int; "
    "  public constructor(lo: int, hi: int) this.lo := lo; this.hi := hi; cur := lo - 1; end constructor; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end RngEnum; "
    "public class Rng: IEnumerable<int> "
    "  private lo: int; private hi: int; "
    "  public constructor(lo: int, hi: int) this.lo := lo; this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return new RngEnum(lo, hi); end GetEnumerator; "
    "end Rng; "
    "public class Bag "
    "  private owned _r: Rng; "
    "  public constructor() _r := new Rng(1, 3); end constructor; "
    "  public method GetEnumerator(): owned IEnumerator<int> return _r.GetEnumerator(); end GetEnumerator; "
    "end Bag; ";

// One 301-pass outer loop; the inner `for` is the defect's spelling. `s` is
// summed into `acc` so the body cannot be constant-folded away, and stdout
// carries the untruncated total.
std::string amplified(const char* moduleName, const std::string& decls,
                      const char* innerFor) {
    return std::string("module ") + moduleName + "; " + decls +
           "public class M "
           "  public static method Main(): int "
           "    var acc := 0; "
           "    for var i in 0 .. 300 do "
           "      var s := 0; " + innerFor +
           "      acc := acc + s; "
           "    end; "
           "    Console.WriteLine($\"{acc}\"); "
           "    return acc mod 100; "
           "  end Main; end M;";
}

JitResult checked(const std::string& prog) { return runAotChecked(prog); }

// 301 x (1 + 2 + 3) = 1806. Residue 6, under 256.
constexpr int kResidueOf1806 = 6;
// 301 x 1 (a break on the first element of every pass) = 301. Residue 1.
constexpr int kResidueOf301 = 1;

}  // namespace

// ===========================================================================
// THE PINNING ROW. `for var x in new FreshSelf(3)` — an UNNAMED temporary
// source on a class that returns a FRESH instance of its own type.
//
// At the fix's parent: `Direct leak of 7224 byte(s) in 301 object(s)`, the run
// exits nonzero, so the row fails on `exitCode`. With the fix: no sanitizer
// finding, residue 6.
//
// AMPLIFIED 301x ON PURPOSE, BUT NOT BECAUSE A ONE-SHOT WOULD MISS IT. This
// comment originally said a one-shot row "would sit inside LSan's
// conservative-scan jitter and could pass on both sides". THAT WAS WRONG and
// is struck out here rather than left to contradict the correction at the
// one-shot row below, which carries the measurement:
//
//     AMPLIFIED 301x ON PURPOSE. The defect is 24 B per pass; a one-shot row would
//     sit inside LSan's conservative-scan jitter and could pass on both sides.
//
// MEASURED: the one-shot DOES catch it on its own (`fresh=6`, then
// `Direct leak of 24 byte(s) in 1 object(s)`, rc=1), and it is one of the four
// rows that go red at the fix's parent. Amplification is kept because the byte
// arithmetic (7224 B in 301 allocations = 24 B/pass) is what makes the finding
// unambiguous on the record — not because the unamplified form is unreliable.
//
// ORACLE. `runAotChecked` is ALWAYS ASan and yields a nonzero exit on ANY
// sanitizer finding, so the leak fails the RUN rather than merely going
// unmentioned — the leak oracle and the ASan oracle agree, which is not true
// of every leak row in this codebase.
// ===========================================================================
BEZEL_TEST(forin_fresh_self_source_disposal_6378,
           fresh_self_unnamed_source_is_disposed_spec_28_9) {
    JitResult r = checked(amplified("M6378A", kFreshSelf,
                                    "for var x in new FreshSelf(3) do s := s + x; end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf1806);
    // 301 x 6 = 1806 — the loop genuinely ran, so a constant-folded body
    // cannot make this row vacuously clean.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1806") != std::string::npos);
}

// THE INHERITED SPELLING of the same defect. `FreshDerived` inherits
// `GetEnumerator`, so the structural scan picks the BASE and `enumIsTsOrBase`
// holds only through #6376's ancestor walk — yet the two pointers still
// differ and the source still leaked. Green on both sides would mean the fix
// only handled the direct spelling; this is the row that distinguishes
// "registered the source when the runtime compare says it is distinct" from
// "special-cased `enumTs == ts`".
BEZEL_TEST(forin_fresh_self_source_disposal_6378,
           inherited_fresh_self_unnamed_source_is_disposed_spec_29_11) {
    JitResult r = checked(amplified(
        "M6378B", std::string(kFreshSelf) + kFreshDerived,
        "for var x in new FreshDerived(3) do s := s + x; end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf1806);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1806") != std::string::npos);
}

// THE CONTROL THAT ESTABLISHES WHICH OBJECT LEAKS. The byte-identical class,
// the same 301 passes, but the temporary BOUND TO A NAME: `var f := new
// FreshSelf(3); for var x in f do … end`. A named source is disposed at its own
// scope end, so it never needed the loop's registration — and it is LSan-CLEAN
// at the fix's PARENT (measured). So the row is the evidence that the 24 B is
// the SOURCE and not the enumerator, contradicting the issue's own guess.
//
// This row is what makes the pinning row non-vacuous: if the leak were the
// enumerator, this control would leak too, and the fix's premise would be
// wrong.
BEZEL_TEST(forin_fresh_self_source_disposal_6378,
           fresh_self_named_source_is_disposed_by_its_own_scope_spec_28_9) {
    JitResult r = checked(amplified(
        "M6378C", kFreshSelf,
        "var f := new FreshSelf(3); for var x in f do s := s + x; end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf1806);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1806") != std::string::npos);
}

// A BREAK out of the fresh-self loop reaches the same endBB disposal — the
// `disposeStack_` scope is popped by the break path — so the SOURCE must be
// reclaimed there too, not only on normal exhaustion. This is the row that
// pins the DISPOSAL SCOPE: a fix that registered the source only on
// `MoveNext() == false` would pass both pinning rows and leak this one.
// `acc` counts one break per pass, so 301, residue 1.
BEZEL_TEST(forin_fresh_self_source_disposal_6378,
           fresh_self_break_reaches_the_same_source_disposal_spec_29_11) {
    JitResult r = checked(amplified("M6378D", kFreshSelf,
                                    "for var x in new FreshSelf(3) do s := s + 1; break; end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf301);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("301") != std::string::npos);
}

// THE ALIASING DIRECTION — THE UNNAMED SELF-RETURNING SOURCE. Here
// `enumPtr == srcPtr`, so the new source registration MUST be suppressed by
// the runtime compare. Deleting that compare (mutation M1) turns this row
// RED with `heap-use-after-free`, which is the whole reason the compare
// exists: the source and the enumerator are one object here, and disposing it
// twice reclaims the block twice. Green on BOTH sides of this change.
//
// This row and the two above have DISJOINT failure sets under the two
// mutations (M1 reddens this one alone; M2 reddens the pinning rows alone),
// so neither half of the fix is decoration.
BEZEL_TEST(forin_fresh_self_source_disposal_6378,
           unnamed_self_returning_source_is_disposed_exactly_once_spec_29_11) {
    JitResult r = checked(amplified("M6378E", kSelfPair,
                                    "for var x in new SelfBase(3) do s := s + x; end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf1806);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1806") != std::string::npos);
}

// #6376's own bare-temporary row, restated: the INHERITED self-returning
// idiom (`for var x in new SelfDerived(3)`). `enumIsTsOrBase` holds through the
// ancestor walk and `enumPtr == srcPtr` holds at runtime, so this is the
// intersection of #6376's guard and this fix's compare. Deleting the compare
// reddens it with `heap-use-after-free`. Green on BOTH sides.
BEZEL_TEST(forin_fresh_self_source_disposal_6378,
           unnamed_inherited_self_returning_source_disposes_once_spec_29_11) {
    JitResult r = checked(amplified("M6378F", kSelfPair,
                                    "for var x in new SelfDerived(3) do s := s + x; end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf1806);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1806") != std::string::npos);
}

// THE NAMED ALIASING SOURCE. `var f := new SelfBase(3); for var x in f` — the
// `#6376` spelling. `sourceIsNamedOwner` is true and `enumPtr == srcPtr`, so
// #2384's / #6376's `ien.selfen` guard is the one that suppresses disposal and
// the fix does not touch this path at all. Green on BOTH sides: a fix that had
// also registered the source here would double-free, so this row is the
// boundary the fix must NOT cross.
BEZEL_TEST(forin_fresh_self_source_disposal_6378,
           named_self_returning_source_still_disposes_once_spec_28_9) {
    JitResult r = checked(amplified(
        "M6378G", kSelfPair,
        "var f := new SelfBase(3); for var x in f do s := s + x; end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf1806);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1806") != std::string::npos);
}

// THE TWO-CLASS IDIOM, UNNAMED SOURCE. `enumIsTsOrBase` is FALSE here, so the
// source registration fired BEFORE this change and must still fire — now behind
// a runtime compare that evaluates false. A fix that had made the compare's
// polarity the wrong way round, or dropped the registration for this case, would
// leak 24 B here; a fix that registered it twice would double-free.
BEZEL_TEST(forin_fresh_self_source_disposal_6378,
           two_class_unnamed_source_is_disposed_spec_29_1) {
    JitResult r = checked(amplified("M6378H", kTwoClass,
                                    "for var x in new Rng(1, 3) do s := s + x; end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf1806);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1806") != std::string::npos);
}

// #6372's DELEGATING shape with an UNNAMED source: `for var x in new Bag()`.
// `Bag` is not itself an enumerator, so `enumIsTsOrBase` is false; the wrapper
// the loop mints is in §29.11 iterator-vtable format and #6372's family
// discriminator frees it. Both halves are exercised at once, and both must keep
// working. Green on BOTH sides of this change.
BEZEL_TEST(forin_fresh_self_source_disposal_6378,
           delegating_unnamed_source_is_disposed_spec_29_1) {
    JitResult r = checked(amplified("M6378I", kTwoClass,
                                    "for var x in new Bag() do s := s + x; end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf1806);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1806") != std::string::npos);
}

// THE NAMED TWO-CLASS SOURCE — the boundary case for the registration's
// `sourceIsNamedOwner` gate, and the spelling #2424 introduced the
// registration for. Green on BOTH sides: a fix that registered a named source
// as well would double-free it at the loop end and again at its own scope end.
BEZEL_TEST(forin_fresh_self_source_disposal_6378,
           two_class_named_source_is_disposed_by_its_own_scope_spec_28_9) {
    JitResult r = checked(amplified(
        "M6378J", kTwoClass,
        "var f := new Rng(1, 3); for var x in f do s := s + x; end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf1806);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("1806") != std::string::npos);
}

// THE ONE-SHOT, UNAMPLIFIED pinning spelling — and, measured, it DOES catch
// the defect on its own. My first draft of this comment claimed a one-shot
// 24-byte finding would sit inside LSan's conservative-scan jitter and could
// pass on both sides; that was WRONG. The same wrong claim also appeared on the
// pinning row above; it is QUOTED AND STRUCK OUT there, not left standing, so
// this file does not contain both the claim and its correction without a
// cross-reference. MEASURED at the fix's parent on exactly this program:
//
//   fresh=6
//   Direct leak of 24 byte(s) in 1 object(s)
//   SUMMARY: AddressSanitizer: 24 byte(s) leaked in 1 allocation(s)
//   rc=1
//
// So the amplified rows above are not the ONLY rows that redden at the parent;
// this one does too. They are still the right primary oracle — the amplified
// count (7224 B in 301 allocations) is what makes the finding UNAMBIGUOUS and
// puts the byte arithmetic on the record — but the one-shot is a genuine third
// pinning row, not merely a value row. It asserts `r.ran` and `exitCode == 0`
// as well as stdout, so a fix that disposed the right OBJECT of the wrong
// THING, or printed 6 by coincidence, still fails here.
BEZEL_TEST(forin_fresh_self_source_disposal_6378,
           one_shot_fresh_self_source_still_yields_every_element_spec_29_11) {
    JitResult r = checked(std::string("module M6378K; ") + kFreshSelf +
                          "public class M "
                          "  public static method Main(): int "
                          "    var s := 0; "
                          "    for var x in new FreshSelf(3) do s := s + x; end; "
                          "    Console.WriteLine($\"fresh={s}\"); "
                          "    return 0; "
                          "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("fresh=6") != std::string::npos);
}