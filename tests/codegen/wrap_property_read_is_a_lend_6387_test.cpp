// Codegen — #6387 / §28.9 + §28.6 + §28.2 (v0.338).
//
// THE DEFECT. `wrapAsIEnumerableForTarget`'s user-class arm
// (src/codegen/LlvmBackend_Assign.cpp:9719) decides the wrapper's slot-3
// `ownsSrc` flag — whether the §29.2 wrapper reclaims its SOURCE — from
// `ctx_.exprSymbol(*srcExpr)`. Its `PropertySymbol` arm asked
// `sema::isAutoPropertyStorageSymbol`, which answers "does this property have
// compiler-generated backing storage?" Its NO answer was then read here as
// "this read produces a FRESH value the wrapper may own". For a HAND-WRITTEN
// getter those are two different questions, so the flag was minted `true` and
// the wrapper freed a field its holder still owns.
//
// §28.9 (v0.338) is decisive and says so in one sentence: "a property or
// indexer **read** is a lend for every declared type … a getter never transfers
// a reclamation obligation to its reader." §28.2 (v0.338) completes it —
// "Every class instance is reclaimed by compiler-inserted code at its holder's
// release point … and nowhere else." The holder is `Holder`, which still owns
// the field, so the wrapper must reclaim nothing.
//
// There is NO "fresh value" counter-case to preserve, and this is MEASURED, not
// argued: sema REJECTS a getter that returns a freshly-produced owned value —
//   "a property or indexer getter may not return a freshly-produced owned
//    value — a getter is a borrow-producing position (properties and indexers
//    do not carry `owned`), so the value would escape with no owner and leak;
//    expose the factory as a method returning `owned T` (§28.6)"
// (MEASURED on p_e.bzl: `get => new SelfA(3);` does not compile.) So a getter's
// result is always a lend of storage somebody else already keeps alive, and the
// auto-property sub-case is SUBSUMED by the widening rather than narrowed: an
// auto-property is a `PropertySymbol` too, and its answer does not move.
//
// NOT #6382. #6382's was a CONSUMER reading the per-instance `ownsSrc` flag
// correctly and acting on it wrongly (a clone disposing a source it does not
// own). This is the PRODUCER minting the flag's VALUE. The discriminator
// MEASURED on this file's parent is exact and is reproduced in the pinning row:
//   at this file's parent  heap-use-after-free, READ of size 8,
//                          in `Holder$DisposeCore`, freed by
//                          `bz$userenumenum$SelfA$SelfA$Dispose`,
//                          allocated in `Holder$ctor`;
//   #6382's              heap-use-after-free, READ of size 1,
//                          in `SelfA$Dispose` (the disposed-flag load).
// Different reading function, different size, different free site.
//
// ORACLE. `runAotChecked`, ALWAYS ASan, nonzero exit on ANY finding. The PLAIN
// leg is UNRELIABLE here — the wrapper frees the source at the END of the
// `for`, and only the holder's teardown touches the freed block afterwards, so
// a non-instrumented run can print the right answer and exit 0. Each row loops
// 200× so the freed block is reused rather than merely faulting once.
//
// NO ROW HERE ASSERTS THAT A LEAK OR A USE-AFTER-FREE IS PRESENT. The defect
// is the absence of a finding: each row asserts the program RUNS CLEAN, which
// is what stops inverting the moment the defect is fixed.
//
// SCOPE — READ THIS BEFORE ASSUMING THIS FILE IS THE WHOLE FIX. The predicate
// this fix corrects is written out FOUR times, and this commit changes ONE of
// them (the one this file's own source lives in). The other three are in
// files this lane does not own:
//   * src/codegen/LlvmBackend_Setup.cpp:18655  — the LOCAL-DECL coercion
//     producer (`var e: IEnumerable<int> := h.Prop;`). MEASURED RED at this
//     file's parent: same `Holder$DisposeCore` /
//     `bz$userenumenum$SelfA$SelfA$Dispose` signature. STILL RED after this
//     commit.
//   * src/codegen/LlvmBackend_Calls2.cpp:9778  — the `.GetEnumerator()`
//     receiver producer. Carries the identical predicate verbatim.
//   * src/codegen/LlvmBackend_Stmts.cpp:9863 and its TWIN — the for-in
//     CONSUMER's `sourceIsNamedLocal` (the same predicate appears twice in
//     that file; the twin is the splice hazard). MEASURED RED at this file's
//     parent via the direct `for var x in h.Prop` read. STILL RED after this
//     commit.
// The fix is one edit in each of the four files, and the same §28.9 sentence
// settles all four. Filed for a follow-up; see the commit body.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The source class, from #6382's `kSelfA` verbatim: it DECLARES the enumerator
// protocol itself, so `wrapUserClassAsIEnumerable`'s structural scan picks
// `enumTs == srcTs` and the wrapper's slot-1 disposer resolves. Verbatim reuse
// is deliberate — it makes this file's rows comparable with #6382's, and it is
// NOT the #6382 mirror (`GetEnumerator` returns `this` and the shape that
// mattered there was a clone disposing it, which this fix does not touch).
const char* kSelfA =
    "public class SelfA: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end SelfA; ";

// A `Holder` whose computed `Seq` getter is what the §29.2 wrap is minted on.
// `Seq`'s declared type is the `IEnumerable<T>` sentinel and its getter body
// widens `SelfA` to it, which is the RETURN edge that reaches
// `wrapAsIEnumerableForTarget`'s user-class arm.
//
// `kSrcProperty` — the source expression IS a property read (`Raw`, a
// hand-written getter over an `owned` field). The defect spelling.
// `kSrcField` — the source expression is the FIELD itself. Same arm, same
// wrap, `FieldSymbol` instead of `PropertySymbol`; GREEN at the base, and it is
// what proves the fix did not disturb the arm's Local/Parameter/Field
// classification.
const char* kSrcProperty =
    "  private owned _inner: SelfA; "
    "  public constructor() _inner := new SelfA(3); end constructor; "
    "  public property Raw: SelfA get => _inner; end Raw; "
    "  public property Seq: IEnumerable<int> "
    "    get => do "
    "      return Raw; "
    "    end; "
    "  end Seq; ";

const char* kSrcField =
    "  private owned _inner: SelfA; "
    "  public constructor() _inner := new SelfA(3); end constructor; "
    "  public property Seq: IEnumerable<int> "
    "    get => do "
    "      return _inner; "
    "    end; "
    "  end Seq; ";

// 200 iterations: the holder is torn down at each loop iteration's scope end, so
// the free the wrapper performs and the read the holder's teardown performs are
// 200 distinct events rather than one.
const char* kMain =
    "  public static method Main(): int "
    "    var s := 0; "
    "    for var i in 0..<200 do "
    "      var h := new Holder(); "
    "      for var x in h.Seq do s := s + x; end; "
    "    end for; "
    "    Console.WriteLine($\"s={s}\"); "
    "    return 0; "
    "  end Main; ";

// Every row returns 0 and prints `s=1200` (200 × (1+2+3)), so `exitCode == 0`
// is a REAL assertion on a run that genuinely ran: the loops are what make the
// free-then-read sequence reachable at all, so a body that never executed could
// not make any of these rows vacuously clean.
JitResult checked(const std::string& prog) { return runAotChecked(prog); }

}  // namespace

// ===========================================================================
// THE PINNING ROW — the defect spelling.
//
// MEASURED at this fix's parent (`r31-p-6382y` @ cea31202) with
// `build/bezelc --sanitize=address` on this exact program:
//
//   ==NNNN==ERROR: AddressSanitizer: heap-use-after-free on address 0x…,
//   READ of size 8 at 0x… thread T0
//       #0 … in bz$TP.Holder$DisposeCore
//       #1 … in bz$TP.Holder$Dispose
//       #2 … in main
//   0x… is located 0 bytes inside of 24-byte region […)
//   freed by thread T0 here:
//       #0 … in free
//       #1 … in bz$userenumenum$SelfA$SelfA$Dispose
//   previously allocated by thread T0 here:
//       #0 … in calloc
//       #1 … in bz$TP.Holder$ctor
//   SUMMARY: AddressSanitizer: heap-use-after-free … in bz$TP.Holder$DisposeCore
//
// The read is `Holder$DisposeCore` touching the block its OWN `owned` field
// names; the free is the ORIGINAL wrapper reclaiming (not a clone, and not a
// foreign source) — which is precisely what distinguishes #6387 from #6382.
// With the fix the same program prints `s=1200` and exits 0 with no finding.
//
// The fail-without leg was run MECHANICALLY, by reverting only the predicate
// and rebuilding: the site logged `srcIsNamedOwner = 0` and the row went RED
// with the signature above; restoring `true` logged `srcIsNamedOwner = 1` and
// the row went GREEN.
// ===========================================================================
BEZEL_TEST(wrap_property_read_is_a_lend_6387,
           computed_property_read_source_wrapper_must_not_reclaim_spec_28_9) {
    JitResult r = checked("module TP; " + std::string(kSelfA) +
                          "public class Holder " + kSrcProperty +
                          "end Holder; "
                          "public class TPImpl " + kMain + "end TPImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 × (1 + 2 + 3) — the loop genuinely ran.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// THE CONTROL ROW — the same wrap, the same arm, the same holder, but the
// source expression is the FIELD rather than a property read. `FieldSymbol`
// answered `srcIsNamedOwner = true` before the fix and answers `true` after, so
// this row is GREEN on both sides by construction. It is here because a fix
// that reached past the `PropertySymbol` arm — to the Local/Parameter/Field
// classification above it, or to the `wrapUserClassAsIEnumerable` call itself —
// would move this row, and nothing else in the file would notice.
BEZEL_TEST(wrap_property_read_is_a_lend_6387,
           field_read_source_control_stays_green_spec_28_2) {
    JitResult r = checked("module TF; " + std::string(kSelfA) +
                          "public class Holder " + kSrcField +
                          "end Holder; "
                          "public class TFImpl " + kMain + "end TFImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}