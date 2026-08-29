// Codegen — issue #3554 (spec §29.7 "Disposal of partially-consumed
// iterators", §29.8 "Iterators with `owned` parameters", §28.3 "Local
// variables, ownership, and borrows", v0.283): ONE plain non-class local
// anywhere in an `IOwningEnumerable<T>` iterator body disarmed the state-keyed
// owned-local disposal for the WHOLE body.
//
//     public static method Gen(v: int): IOwningEnumerable<Box>
//       var q := 8;                 /* the only change */
//       var a := new Box(1);
//       yield return new Box(2);
//       yield return C.Boom(v);     /* throws */
//       yield return a;
//     end Gen;
//
//     $ bezelc --sanitize=address -o p p.bzl && ./p
//     caught=200
//     live=400 dead=200
//     ERROR: LeakSanitizer: detected memory leaks
//     Direct leak of 3200 byte(s) in 200 object(s) allocated from:
//         #1 bz$iter$Gen$MoveNext
//
// Without `var q := 8;` the identical body is `live=400 dead=400`, clean.
//
// ── WHAT WAS WRONG. `emitIteratorMethod`'s post-scan filter
// (`LlvmBackend_Iterators.cpp`) read
//
//     for (auto& [ld, pr] : lscanOut) {
//       if (pr.second == 0)              { allTransferred = false; break; }
//       if (!unwindOwnedLocalClass(ld))  { allTransferred = false; break; }
//     }
//     ... else ownedLocalYieldStates.clear();
//
// `collectYieldStatesForOwnedLocals` registers EVERY plain non-`using`,
// non-`autoDropAtScopeEnd` local with NO type gate — deliberately, because the
// gate belongs at emit time where the frame and the disposer table exist. So
// `var q := 8;` lands in the map with `transferredAtState == 0`, trips the
// FIRST test, and the `else` discards the whole map — including the correctly
// classified `a`. Not one leak per unclassifiable local: one leak per body.
//
// The remedy is to make the veto PROPORTIONAL, by running the TYPE GATE FIRST:
// a local that is not a dispose-via-vtable-slot-2 class/interface instance
// carries no disposal obligation on this path at all, so skipping it cannot
// produce a double free and it is not evidence of ambiguity. The genuinely
// ambiguous case — a local that IS an owned class instance yet was never
// transferred at a `yield return L;`, so its discharge sits at a site the
// linear scan cannot statically place (§29.6 catch/finally bodies are not
// walked) — deliberately keeps the conservative whole-body bail-out.
//
// ── DIRECTION. This ARMS a disposal that was not running, so the risk is an
// over-release, not a leak. Every row is `runAotChecked` (generated-code ASan
// with LeakSanitizer on), so a double free or use-after-free fails the row
// before the printed balance is compared, and the `live=/dead=` balance is a
// second, independent oracle on the same run. Rows 4 and 5 are the CONTROLS:
// row 4 drives the SAME body to exhaustion, where `a` really is transferred and
// a newly-armed disposal would be a double free; row 5 is the same iterator
// with no plain local at all, which was already green and must stay green.
// Together they are what says the proportional veto did not over-reach.
//
// Every row is amplified to 200 enumerations: a one-shot LSan row can pass on
// both sides because the allocation is still stack-reachable at exit.
// per §29.7, §29.8, §28.3 (issue #3554)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a leak, a double free or a use-after-free fails the row before the
// printed balance is compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// Shared prelude: a counted `Box` plus the throwing producer and the manual
// `GetOwningEnumerator` driver. `Drive` returns 1 on the caught throw so the
// printed `caught=` line proves all 200 activations really took the throw edge
// (a row whose iterator silently stopped yielding would still balance).
const std::string kPrelude =
    "module M; import System; import System.Collections.Generic; "
    "class Box "
    "  public static live: int := 0; "
    "  public static dead: int := 0; "
    "  public Id: int := 0; "
    "  public constructor(id: int) Id := id; Box.live := Box.live + 1; end constructor; "
    "  destructor Box.dead := Box.dead + 1; end destructor; "
    "end Box; "
    "class C "
    "  public static method Boom(v: int): owned Box "
    "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
    "    return new Box(99); "
    "  end Boom; ";

const std::string kDriver =
    "  public static method Drive(v: int): int "
    "    do "
    "      using var source: IOwningEnumerable<Box> := C.Gen(v); "
    "      using var en: IOwningEnumerator<Box> := source.GetOwningEnumerator(); "
    "      try "
    "        while en.MoveNext() do "
    "          using var u := en.TakeCurrent(); "
    "        end; "
    "      catch e: InvalidOperationException => "
    "        return 1; "
    "      end; "
    "    end; "
    "    return 0; "
    "  end Drive; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    for var j in 0..<200 do caught := caught + C.Drive(3); end; "
    "    Console.WriteLine(\"caught=\" + caught.ToString()); "
    "    Console.WriteLine(\"live=\" + Box.live.ToString() + \" dead=\" + Box.dead.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// Same driver, but `v = 1` so `Boom` returns instead of throwing and the
// iterator is driven to exhaustion.
const std::string kDriverExhaust =
    "  public static method Drive(v: int): int "
    "    do "
    "      using var source: IOwningEnumerable<Box> := C.Gen(v); "
    "      using var en: IOwningEnumerator<Box> := source.GetOwningEnumerator(); "
    "      try "
    "        while en.MoveNext() do "
    "          using var u := en.TakeCurrent(); "
    "        end; "
    "      catch e: InvalidOperationException => "
    "        return 1; "
    "      end; "
    "    end; "
    "    return 0; "
    "  end Drive; "
    "  public static method Main(): int "
    "    var caught := 0; "
    "    for var j in 0..<200 do caught := caught + C.Drive(1); end; "
    "    Console.WriteLine(\"caught=\" + caught.ToString()); "
    "    Console.WriteLine(\"live=\" + Box.live.ToString() + \" dead=\" + Box.dead.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";
}  // namespace

// ── Row 1, the issue's repro: one plain `int` local declared BEFORE the owned
// local. Pre-fix `live=400 dead=200` + `3200 byte(s) in 200 object(s)` leaked
// from `bz$iter$Gen$MoveNext`; the owned `a` of every activation was never
// disposed because `q` had vetoed the whole body's analysis.
// per §29.7, §28.3 (issue #3554)
BEZEL_TEST(iterator_plain_local_disarms_owned_local_disposal_3554,
           plain_int_local_before_the_owned_local_does_not_disarm_disposal_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        kPrelude +
        "  public static method Gen(v: int): IOwningEnumerable<Box> "
        "    var q := 8; "
        "    var a := new Box(1); "
        "    yield return new Box(2); "
        "    yield return C.Boom(v); "
        "    yield return a; "
        "  end Gen; " +
        kDriver),
        "caught=200\nlive=400 dead=400\n");
}

// ── Row 2: the plain local declared BETWEEN two yields, so its own
// `establishedAtState` is 2 rather than 1. The veto did not depend on where the
// unclassifiable local sat — any position disarmed the body — and the fix must
// not depend on it either.
// per §29.7, §28.3 (issue #3554)
BEZEL_TEST(iterator_plain_local_disarms_owned_local_disposal_3554,
           plain_int_local_between_yields_does_not_disarm_disposal_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        kPrelude +
        "  public static method Gen(v: int): IOwningEnumerable<Box> "
        "    var a := new Box(1); "
        "    yield return new Box(2); "
        "    var q := 7; "
        "    yield return C.Boom(v); "
        "    yield return a; "
        "  end Gen; " +
        kDriver),
        "caught=200\nlive=400 dead=400\n");
}

// ── Row 3: TWO owned locals transferred at DIFFERENT later yields, alongside
// the plain local. This is the row that says the surviving map is the whole
// correctly-classified set and not just its first entry: both `a` (transferred
// at yield 3) and `b` (yield 4) are live at the parked suspension state 1 and
// both must be disposed. Per activation: 3 constructed (a, b, the yielded
// Box(3)) and 3 disposed — the yielded one by the consumer's `using var u`,
// `a` and `b` by the iterator's own Dispose. x200 = 600 / 600.
// per §29.7, §29.8, §28.3 (issue #3554)
BEZEL_TEST(iterator_plain_local_disarms_owned_local_disposal_3554,
           two_owned_locals_survive_the_plain_local_in_the_same_body_spec_29_8) {
    BEZEL_EXPECT_EQ(checkedOut(
        kPrelude +
        "  public static method Gen(v: int): IOwningEnumerable<Box> "
        "    var q := 8; "
        "    var a := new Box(1); "
        "    var b := new Box(2); "
        "    yield return new Box(3); "
        "    yield return C.Boom(v); "
        "    yield return a; "
        "    yield return b; "
        "  end Gen; " +
        kDriver),
        "caught=200\nlive=600 dead=600\n");
}

// ── Row 4, the OVER-RELEASE control: the same body as row 1, driven with
// `v = 1` so nothing throws and the iterator runs to EXHAUSTION. `a` really is
// transferred at yield 3 and the consumer's `using var u` disposes it, so the
// newly-armed state-keyed visit must NOT fire for it — if the proportional veto
// had armed a disposal that the transfer already discharged, this row is a
// double free, which ASan reports as a heap-use-after-free before the balance
// is read. `caught=0` proves the throw edge was genuinely not taken, so the row
// is measuring the exhaustion path and not silently repeating row 1.
// Per activation 3 boxes are constructed (a, the yielded Box(2), Boom's
// Box(99)) and all 3 are disposed by the consumer. x200 = 600 / 600.
// per §29.7, §28.3 (issue #3554)
BEZEL_TEST(iterator_plain_local_disarms_owned_local_disposal_3554,
           control_exhaustion_does_not_double_dispose_the_transferred_local_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        kPrelude +
        "  public static method Gen(v: int): IOwningEnumerable<Box> "
        "    var q := 8; "
        "    var a := new Box(1); "
        "    yield return new Box(2); "
        "    yield return C.Boom(v); "
        "    yield return a; "
        "  end Gen; " +
        kDriverExhaust),
        "caught=0\nlive=600 dead=600\n");
}

// ── Row 5, the NO-CHANGE CONTROL: the identical iterator with NO plain local.
// This shape
// was already green before the fix (`live=400 dead=400`, clean) and is what
// says the proportional veto did not over-reach into a double dispose — under
// ASan an over-release here is a heap-use-after-free, not a wrong count.
// per §29.7, §28.3 (issue #3554)
BEZEL_TEST(iterator_plain_local_disarms_owned_local_disposal_3554,
           control_no_plain_local_still_disposes_exactly_once_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        kPrelude +
        "  public static method Gen(v: int): IOwningEnumerable<Box> "
        "    var a := new Box(1); "
        "    yield return new Box(2); "
        "    yield return C.Boom(v); "
        "    yield return a; "
        "  end Gen; " +
        kDriver),
        "caught=200\nlive=400 dead=400\n");
}
