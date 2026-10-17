// Codegen — #6397 / §28.9 + §28.2 (v0.338). The two for-in CONSUMER sites in
// src/codegen/LlvmBackend_Stmts.cpp, and the mints they have to be able to
// read.
//
// THE DEFECT. `lowerIEnumerableBuiltinFor` and `lowerIEnumerableFor` each decide
// `sourceIsNamedLocal` / `sourceIsNamedOwner` — "is this source a NAMED binding
// that disposes itself, so THIS LOOP MUST NOT reclaim it?" — and both spelled
// their `PropertySymbol` arm with `sema::isAutoPropertyStorageSymbol`, which
// answers a different question: "does this property have compiler-generated
// backing storage?". Its NO answer was read here as "this read produces a FRESH
// value the loop owns", so a hand-written getter (`get => _a`) got a loop-end
// disposal registered on the holder's own field.
//
// §28.9 (v0.338) settles the SOURCE half in one sentence: "a property or
// indexer **read** is a lend for every declared type … a getter never transfers
// a reclamation obligation to its reader." §28.2 (v0.338) completes it — an
// instance "is reclaimed by compiler-inserted code at its holder's release
// point … and nowhere else."
//
// WHAT THE FIX ACTUALLY IS, because the sentence above is not the whole answer.
//
// (1) The SOURCE is not always a lend, and the compiler already knows which
//     case it is — but only the half that goes through a §29.2 wrap. The
//     wrap-creator mints a fresh wrapper FRAME for a getter that WIDENS its
//     body to the declared head (`property Items: IEnumerable<int> get =>
//     _items;` over `owned _items: Array<int>`), and that frame is held by
//     nobody but the reader. `propReadIterWrappedExprs_` records exactly that —
//     "this read minted a frame", inserted on a mint, erased on a decline —
//     and both consumer sites now read it instead of re-deriving the verdict
//     from a name list.
//
// (2) The block-bodied getter's mint was published NOWHERE.
//     `propReadIterWrappedExprs_` has exactly two writers and both are inside
//     `wrapPropertyArrayAsIEnumerable`, the EXPRESSION-bodied creator. A
//     block-bodied accessor is emitted as a separate, memoised function
//     (`ensurePropertyBlockGetter`) and its mint fires on the RETURN edge
//     (`lowerReturn`), so the node available there is the accessor's own
//     `return` — not the caller's read node, which the accessor never sees and,
//     once the memo hits, can no longer reach. MEASURED: with the read-expr
//     consumer and no publisher for that family, `wrap_property_read_is_a_lend
//     _6387`'s two landed rows AND `codegen_block_bodied_ienumerable_property
//     _bound_disposal_2769`'s direct-for-in row go RED with `Direct leak of
//     6400 byte(s) in 200 object(s) allocated from bz$prop$Seq$get`. This
//     commit publishes that verdict, keyed on the PropertyDecl (see
//     `propBlockGetterReturnKinds_` in LlvmBackend_impl.hpp for why the two
//     families cannot share one key).
//
// (3) `wrapAsIEnumerableForTarget`'s USER-CLASS arm never reported its mint.
//     Every other arm sets `outWrapMade`; that one is a tail `return`. It is
//     not cosmetic: the user-class family is exactly what repro B's
//     block-bodied getter mints, so with the flag stuck `false` the frame went
//     unpublished even once the channel existed.
//
// (4) "No frame minted" is NOT the same answer as "the reader does not own
//     it". #1840's landed rows pin a getter that PRODUCES a value — `public
//     property Seq: IEnumerable<int> => C.Gen();` over a yielding `Gen()` —
//     and the iterator `Gen()` allocates belongs to the LOOP. A consumer keyed
//     on the mint alone classifies that read as a lend and
//     `forin_computed_property_ienumerable_1840`'s member-access row goes RED
//     with `Direct leak of 40 byte(s) in 1 object(s) allocated from
//     bz$M.C$Gen`. So the consumer's question is "is the value the receiver's
//     own storage", answered by two published verdicts (a minted frame is the
//     reader's whatever the body is; a getter that hands back a FIELD read
//     hands back storage its holder already keeps alive, §28.2) plus the
//     auto-property answer that was already there. Everything else keeps the
//     classification it had — this fix only ever NARROWS a claim to the case
//     it can prove, never widens one.
//
// BOTH CONSUMER SITES ARE REQUIRED — MEASURED, and this is why the file has two
// pinning rows rather than one. The two functions are the two arms of the
// dispatch at LlvmBackend_Stmts.cpp:4615/4617, and they are selected by the
// source's DECLARED TYPE, not by anything about the property:
//
//   patch applied            repro A (_a: SelfA)        repro B (_a: IEnumerable<int>)
//   ------------------------  -------------------------  -----------------------------
//   neither site (staging)    RED heap-use-after-free    RED heap-use-after-free
//   lowerIEnumerableBuiltinFor only
//                             RED heap-use-after-free    GREEN, s=1200
//   lowerIEnumerableFor only  GREEN, s=1200              RED heap-use-after-free
//   BOTH (this fix)           GREEN, s=1200              GREEN, s=1200
//
// Each leg was run MECHANICALLY: the file was reverted with `git checkout --`,
// ONE hunk re-applied from the saved per-hunk patch, the source `touch`ed so
// the rebuild was real, and both programs re-run under ASan. An implementer
// who patches the first site and treats the second as a copy-paste twin leaves
// repro A red.
//
// ORACLE. `runAotChecked` — ALWAYS ASan, nonzero on ANY finding. The PLAIN leg
// is UNRELIABLE here: the loop-end free happens at the `for`'s exit and only
// the holder's teardown touches the freed block afterwards, so a
// non-instrumented run can print the right answer and exit 0. Every program
// loops so the free-then-read sequence is reached on every iteration rather
// than by luck.
//
// ⚠ LSan IS **OFF** IN `runAotChecked` BY DEFAULT (`aot_support.hpp`'s
// `LeakDetectMode` passes `detect_leaks=0`), so a plain `runAotChecked` row
// CANNOT observe a leak — it exits 0 on a tree that leaks. Rows A–E assert the
// USE-AFTER-FREE, which ASan reports with `detect_leaks=0` and which is what
// this file's original defect was, so they are armed correctly by construction.
// The LEAK rows must arm the knob explicitly with `LeakEnv("strict")` below; a
// leak row that does not is worse than no row, because it looks like coverage
// and certifies nothing. (`todictionary_duplicate_key_leak_5421_test.cpp`
// carries the same save/restore, for the same reason.)
//
// NO ROW HERE ASSERTS THAT A LEAK OR A USE-AFTER-FREE IS PRESENT. Each row
// asserts the program runs clean and prints the right sum, which is what stops
// the rows inverting the moment the defect is fixed.
//
// §-strings live in COMMENTS only; a `§` inside an assertion would make the row
// a tripwire on a diagnostic rename.
#include <cstdlib>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Save/restore BEZEL_AOT_DETECT_LEAKS around a row. Required, not tidiness: the
// gate may run a whole shard with the knob set, and a row that asserts
// knob-ON behaviour must not be polluted by (or pollute) its neighbours.
class LeakEnv {
public:
    explicit LeakEnv(const char* value) {
        const char* prev = std::getenv("BEZEL_AOT_DETECT_LEAKS");
        had_ = prev != nullptr;
        if (had_) prev_ = prev;
        if (value) ::setenv("BEZEL_AOT_DETECT_LEAKS", value, /*overwrite=*/1);
        else       ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    ~LeakEnv() {
        if (had_) ::setenv("BEZEL_AOT_DETECT_LEAKS", prev_.c_str(), 1);
        else      ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    LeakEnv(const LeakEnv&) = delete;
    LeakEnv& operator=(const LeakEnv&) = delete;
private:
    bool had_ = false;
    std::string prev_;
};

// A user class that DECLARES the enumerator protocol itself, so the
// user-class lowering's structural scan resolves an enumerator type and a
// disposer. Its `GetEnumerator` returns `this`, so the object in the loop's
// source slot IS its own enumerator — the shape in which an erroneous
// loop-end source reclamation is immediately read back by the holder's
// teardown. (`hi` bounds the sequence to 1+2+3 = 6.)
const char* kSelfA =
    "public class SelfA: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end SelfA; ";

// REPRO A's holder — the FIELD's declared type is the user class itself, so
// `for var x in h.Seq` selects `lowerIEnumerableFor` (the user-class arm) and
// the value landing in the loop's source slot IS the holder's own `_a`.
const char* kHolderSelfA =
    "public class HolderA "
    "  private owned _a: SelfA; "
    "  public constructor() _a := new SelfA(3); end constructor; "
    "  public property Seq: SelfA get => _a; end Seq; "
    "end HolderA; ";

// REPRO B's holder — the issue's LITERAL spelling: the FIELD is declared
// `IEnumerable<int>`, so the read is an iterator-format property read and the
// loop selects `lowerIEnumerableBuiltinFor` (the builtin arm). Different
// declared-type spelling, different lowering function, same §28.9 question.
const char* kHolderIface =
    "public class HolderB "
    "  private owned _b: IEnumerable<int>; "
    "  public constructor() _b := new SelfA(3); end constructor; "
    "  public property Seq: IEnumerable<int> get => _b; end Seq; "
    "end HolderB; ";

// CONTROL 1 — the source expression is the FIELD, not a property read. The
// `FieldSymbol` arm answered `true` before this fix and answers `true` after,
// so this row is GREEN on both sides by construction. It is here because a fix
// that reached past the `PropertySymbol` arm — to the Local/Parameter/Field
// classification above it, or to the disposal registration the flag gates —
// would move this row and nothing else in the file would notice.
//
// The class body is deliberately left OPEN in the constant, so the `Sum()`
// method that spells the loop over `_f` can be spliced INSIDE it — the read has
// to come from the holder, since `_f` is private.
const char* kHolderField =
    "public class HolderF "
    "  private owned _f: SelfA; "
    "  public constructor() _f := new SelfA(3); end constructor; ";

// (An auto-property source spelling is deliberately NOT defined here — see the
// note above the last control row for the measurement that ruled it out.)

// 200 iterations: the holder is created and torn down inside the loop, so the
// loop-end free and the holder's read of the freed block are 200 distinct
// events rather than one. 200 × (1 + 2 + 3) = 1200.
const char* kMain =
    "  public static method Main(): int "
    "    var s := 0; "
    "    for var i in 0..<200 do "
    "      var h := new %s(); "
    "      for var x in h.Seq do s := s + x; end; "
    "    end for; "
    "    Console.WriteLine($\"s={s}\"); "
    "    return 0; "
    "  end Main; ";

// The main loop, reading `h.Seq` off a freshly constructed holder of the given
// type, with the holder's own declaration spliced in ahead of the driver.
std::string loopOverHolder(const char* holderDecl, const std::string& holderType) {
    std::string m = kMain;
    m.replace(m.find("%s"), 2, holderType);
    return "module TM; " + std::string(kSelfA) + holderDecl +
           "public class TMImpl " + m + "end TMImpl;";
}

// Rows C-E need their own drivers only because their holders expose the
// property under DIFFERENT names (`E`, `Seq`) or none at all, so the shared
// `%s`-templated driver above (which always spells `h.Seq`) cannot be reused
// verbatim. Each is the same 200-iteration shape and each prints the same
// `s=1200`, so the rows stay comparable with A and B.
const char* kMainC =
    "  public static method Main(): int "
    "    var s := 0; "
    "    for var i in 0..<200 do "
    "      var h := new HolderC(); "
    "      for var x in h.E do s := s + x; end; "
    "    end for; "
    "    Console.WriteLine($\"s={s}\"); "
    "    return 0; "
    "  end Main; ";

const char* kMainD =
    "  public static method Main(): int "
    "    var s := 0; "
    "    for var i in 0..<200 do "
    "      var h := new HolderD(); "
    "      for var x in h.Seq do s := s + x; end; "
    "    end for; "
    "    Console.WriteLine($\"s={s}\"); "
    "    return 0; "
    "  end Main; ";

const char* kMainE =
    "  public static method Main(): int "
    "    var s := 0; "
    "    for var i in 0..<200 do "
    "      var h := new HolderE(); "
    "      for var x in h.Seq do s := s + x; end; "
    "    end for; "
    "    Console.WriteLine($\"s={s}\"); "
    "    return 0; "
    "  end Main; ";

JitResult checked(const std::string& prog) { return runAotChecked(prog); }

}  // namespace

// ===========================================================================
// PINNING ROW A — the USER-CLASS arm (`lowerIEnumerableFor`).
//
// §28.9's lend, with the source's declared type spelling the loop through
// `lowerIEnumerableFor`. At this fix's parent, `bezelc --sanitize=address` on
// this exact program:
//
//   ==NNNN==ERROR: AddressSanitizer: heap-use-after-free on address 0x…,
//   READ of size 8 at 0x… thread T0
//       #0 … in bz$TM.HolderA$DisposeCore
//       #1 … in bz$TM.HolderA$Dispose
//       #2 … in main
//   0x… is located 0 bytes inside of 24-byte region […)
//   freed by thread T0 here:
//       #0 … in free
//       #1 … in main                      <- the loop-end disposal, inlined
//   previously allocated by thread T0 here:
//       #0 … in calloc
//       #1 … in bz$TM.HolderA$ctor
//   SUMMARY: AddressSanitizer: heap-use-after-free … in bz$TM.HolderA$DisposeCore
//
// The block freed is the holder's own `owned` field, freed by the loop and read
// back by the holder's teardown. With the fix the same program prints `s=1200`
// and exits 0 with no finding.
//
// The fail-without leg for THIS row is the `lowerIEnumerableFor`-only build
// above: this row is green there and green with both hunks, and RED when this
// file's second hunk alone is reverted.
// ===========================================================================
BEZEL_TEST(for_in_property_read_is_a_lend_6397,
           user_class_typed_property_read_loop_must_not_reclaim_spec_28_9) {
    JitResult r = checked(loopOverHolder(kHolderSelfA, "HolderA"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 × (1 + 2 + 3) — the loop genuinely ran.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// ===========================================================================
// PINNING ROW B — the BUILTIN arm (`lowerIEnumerableBuiltinFor`).
//
// The issue's LITERAL spelling: the holder's field is declared
// `IEnumerable<int>`, so the read is an iterator-format property read and the
// loop dispatches to the builtin arm. MEASURED at this fix's parent with the
// same `bezelc --sanitize=address` invocation:
//
//   ==NNNN==ERROR: AddressSanitizer: heap-use-after-free on address 0x…,
//   READ of size 8 at 0x… thread T0
//       #0 … in bz$TM.HolderB$DisposeCore
//   …
//
// Its fail-without leg is the `lowerIEnumerableBuiltinFor`-only build above:
// this row is RED there and GREEN with both hunks.
// ===========================================================================
BEZEL_TEST(for_in_property_read_is_a_lend_6397,
           ienumerable_typed_property_read_loop_must_not_reclaim_spec_28_9) {
    JitResult r = checked(loopOverHolder(kHolderIface, "HolderB"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// ===========================================================================
// CONTROL — the FIELD-READ source.
//
// Same loop, same holder, same disposal registration the flag gates; only the
// source expression differs (the `FieldSymbol` arm, one branch above the
// widened one). GREEN on both sides by construction, and it is the row that
// would move if the widening had reached past the `PropertySymbol` arm.
// ===========================================================================
BEZEL_TEST(for_in_property_read_is_a_lend_6397,
           field_read_source_control_stays_green_spec_28_2) {
    // The loop reads `h._f` directly — `_f` is private, so the read is spelled
    // from inside a method the holder exposes for exactly this purpose.
    JitResult r = checked(
        "module TC; " + std::string(kSelfA) + kHolderField +
        "  public method Sum(): int "
        "    var t := 0; "
        "    for var x in _f do t := t + x; end; "
        "    return t; "
        "  end Sum; "
        "end HolderF; "
        "public class TCImpl "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<200 do "
        "      var h := new HolderF(); "
        "      s := s + h.Sum(); "
        "    end for; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end TCImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// ===========================================================================
// CONTROL — the FRESH-TEMPORARY source, the direction the widening must NOT
// reach.
//
// The flag this fix widens gates a loop-end RECLAMATION, so the risk of a
// §28.9 widening is over-suppression: claim every property read is a named
// owner and a genuinely unowned temporary loses its only disposal and leaks.
// This row is that case — `for var x in new SelfA(3)` binds the source to no
// name, so the loop is its SOLE owner and must still reclaim it. 200 fresh
// temporaries, so a suppression is 200 leaked 24-byte objects and LSan reports
// them; with the fix the run is clean and the sum is 200 × 6 = 1200.
//
// GREEN at this fix's parent and GREEN with both hunks: the widened arm is
// reached only for a `PropertySymbol` source, and this source is a `NewExpr`.
// It is here to keep the next implementer from widening further.
//
// NOT COVERED HERE, AND DELIBERATELY: an AUTO-PROPERTY source
// (`public property Seq: SelfA;`, the sub-case the old predicate answered
// `true` for — the widening subsumes it rather than narrowing it). That shape
// is NOT usable as a control row here: its holder leaks its OWN backing store,
// `Direct leak of 4800 byte(s) in 200 object(s)` — 24 B each, the `SelfA`
// itself. MEASURED as PRE-EXISTING and INDEPENDENT of this fix: identical
// report at this fix's parent (3ba16717) and with both hunks applied, with
// `var h`, with `using var h`, and with an explicit `h.Dispose()`. An owning
// auto-property's backing store is not in its holder's disposal cascade. That
// is a separate defect, outside this lane's scope; the row was left out rather
// than shipped as a permanent red, and the finding is reported in the commit
// body. The two controls above already bracket the widened arm's boundary —
// `FieldSymbol` stays `true` and an unowned temporary stays `false` — so the
// auto-property sub-case is not left unguarded by its absence.
// ===========================================================================
BEZEL_TEST(for_in_property_read_is_a_lend_6397,
           fresh_temporary_source_is_still_reclaimed_spec_29_11) {
    JitResult r = checked(
        "module TT; " + std::string(kSelfA) +
        "public class TTImpl "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<200 do "
        "      for var x in new SelfA(3) do s := s + x; end; "
        "    end for; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end TTImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}
// ===========================================================================
// PINNING ROW C — the BLOCK-BODIED getter that WIDENS. The family the
// consumer-only half of this fix leaves unpublished.
//
// `E`'s getter body is `_a` (an `Array<int>`) and the declared type is
// `IEnumerable<int>`, so the RETURN edge mints a §29.2 wrapper frame that
// nobody but this loop owns. With no publisher for that family the read is
// classified as a lend and the frame is reclaimed by nobody: MEASURED, the
// same `Direct leak of 6400 byte(s) in 200 object(s)` this file's header
// quotes, and #2769's own direct-for-in row goes RED the same way.
//
// 200 × (1 + 2 + 3) = 1200.
// ===========================================================================
BEZEL_TEST(for_in_property_read_is_a_lend_6397,
           block_bodied_widening_getter_loop_must_reclaim_frame_spec_29_2) {
    JitResult r = checked(
        std::string("module TC; ") +
        "public class HolderC "
        "  private owned _a: Array<int>; "
        "  public constructor() "
        "    _a := new Array<int>(); "
        "    _a.Add(1); _a.Add(2); _a.Add(3); "
        "  end constructor; "
        "  public property E: IEnumerable<int> "
        "    get => do "
        "      return _a; "
        "    end; "
        "  end E; "
        "end HolderC; "
        "public class TCImpl " + kMainC + "end TCImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// ===========================================================================
// PINNING ROW D — the BLOCK-BODIED getter that does NOT widen. The mirror of
// row C, and the row that a "every block-bodied getter mints" rule breaks.
//
// `Seq` is declared `SelfA`, the SAME type its body `_f` already has, so the
// RETURN edge's wrap DECLINES and no frame is ever minted: the value in the
// loop's source slot is the holder's own `owned` field. §28.2 keeps that field's
// reclamation at the holder's release point. A rule that classified this read
// as a claim would free `_f` at the loop's end and the holder's teardown would
// then read the freed block — the same heap-use-after-free row A pins, reached
// from the other direction.
//
// This row is green on both sides of THIS fix; it is here because the two
// verdicts the consumer ORs together are published independently, and a change
// that wired only the mint bit (or that read "there is a block getter" as the
// verdict) would move this row and nothing else in the file.
// ===========================================================================
BEZEL_TEST(for_in_property_read_is_a_lend_6397,
           block_bodied_non_widening_getter_is_a_lend_spec_28_2) {
    JitResult r = checked(
        std::string("module TD; ") + std::string(kSelfA) +
        "public class HolderD "
        "  private owned _f: SelfA; "
        "  public constructor() _f := new SelfA(3); end constructor; "
        "  public property Seq: SelfA "
        "    get => do "
        "      return _f; "
        "    end; "
        "  end Seq; "
        "end HolderD; "
        "public class TDImpl " + kMainD + "end TDImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// ===========================================================================
// CONTROL — the getter that PRODUCES a value. The direction a
// "a property read is a lend" simplification breaks, and the shape #1840's
// landed rows already pin.
//
// `G.Gen()` is a yielding generator, so every read of `Seq` allocates a fresh
// iterator that nothing else can reach and no §29.2 wrap is involved at all —
// the value is already an `IEnumerable<int>`. The reader owns it, and 200 reads
// that nobody reclaims are 200 leaked iterators. A consumer keyed on "did a
// wrap mint a frame" answers "no" here and turns
// `forin_computed_property_ienumerable_1840`'s member-access row RED with
// `Direct leak of 40 byte(s) in 1 object(s) allocated from bz$M.C$Gen` —
// MEASURED on this lane's own tree before the classification was widened.
//
// 200 × (1 + 2 + 3) = 1200.
// ===========================================================================
BEZEL_TEST(for_in_property_read_is_a_lend_6397,
           produced_value_getter_source_is_still_reclaimed_spec_28_3) {
    JitResult r = checked(
        std::string("module TE; ") +
        "class G public static method Gen(): IEnumerable<int> "
        "  yield return 1; yield return 2; yield return 3; "
        "end Gen; end G; "
        "public class HolderE "
        "  public property Seq: IEnumerable<int> => G.Gen(); "
        "end HolderE; "
        "public class TEImpl " + kMainE + "end TEImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// ===========================================================================
// PINNING ROW F — the ARROW getter through a SLOT function (the fix-forward).
//
// THE FACE ROWS A–E DO NOT REACH. Every getter above is read through a
// CONCRETE receiver, which inlines the body at the read site, so all five
// travel the INLINE face: `wrapPropertyArrayAsIEnumerable` is handed the read
// node and records its verdict in `propReadIterWrappedExprs_`. A `virtual`
// (or interface) read has no inlined body — it dispatches through a slot
// function, and the §29.2 wrap fires inside that accessor instead. The
// accessor has no caller read node to key a verdict on, and an arrow body has
// no `return` edge for `lowerReturn` to report, so on this face the frame was
// minted and published NOWHERE.
//
// MEASURED on the composed tree before this fix, with
// `scripts/agent_check.sh --probe --asan` (LSan at DEFAULT settings — no
// `LSAN_OPTIONS` of any kind is needed to see it, and none is set anywhere in
// this file):
//
//   s=1200
//   Direct leak of 6400 byte(s) in 200 object(s) allocated from:
//       #0 … in calloc
//       #1 … in bz$Base$E$propget
//   agent_check: probe run exit=1
//
// 6400 / 200 = 32 bytes = one wrapper frame per read, and the allocating frame
// is the ACCESSOR — the §29.2 wrapper the loop was obliged to reclaim. The
// consumer was told "no mint", took the field-lend branch, and withdrew the
// loop's reclamation.
//
// WHY THE KNOB IS ARMED. `runAotChecked` passes `detect_leaks=0` by default
// (see the file header), so this row is a leak assertion and must arm
// `BEZEL_AOT_DETECT_LEAKS=strict` itself or it would pass on the very tree it
// exists to catch. The `checkedToolchainSkipped` assertion comes FIRST and
// INVERTS the skip, so a toolchain that cannot link the ASan binary fails
// loudly here instead of being absorbed into a green row.
//
// §28.9 makes the two body spellings "semantically indistinguishable at every
// read site", which is why the arrow face owes the reader the verdict the
// block face publishes. §29.2 is the obligation: the desugaring's
// `using var __source` is a HOLDER, so the wrapper frame minted for it is
// reclaimed there and — §28.2, "reclaimed … at its holder's release point …
// and nowhere else" — nowhere else. The holder's own `owned _e` is a holder
// for `_e`, not for the wrapper the accessor minted over it, so nothing else
// in the program stands behind that frame.
//
// 200 × (1 + 2 + 3) = 1200.
// ===========================================================================
BEZEL_TEST(for_in_property_read_is_a_lend_6397,
           virtual_arrow_getter_loop_reclaims_its_minted_frame_spec_29_2) {
    LeakEnv scope("strict");
    JitResult r = checked(
        "module TF; " + std::string(kSelfA) +
        "public class Base "
        "  private owned _e: SelfA; "
        "  public constructor() _e := new SelfA(3); end constructor; "
        "  public virtual property E: IEnumerable<int> get => _e; end E; "
        "end Base; "
        "public class Der: Base end Der; "
        "public class TFImpl "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<200 do "
        "      var b: Base := new Der(); "
        "      for var x in b.E do s := s + x; end; "
        "    end for; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end TFImpl;");
    // Loud BEFORE any absorb: a silently-skipped leak oracle would certify
    // nothing while reporting green.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// ===========================================================================
// CONTROL — the DECLINING arrow getter through a slot function.
//
// The mirror of row F, and the row that would move FIRST if this fix published
// a claim without consulting the wrap creator's own verdict. `E`'s body `_e` is
// already declared `IEnumerable<int>`, so the §29.2 wrap has nothing to widen
// and DECLINES: it returns its input unchanged. No frame is minted, the value
// in the loop's source slot IS the holder's own `owned` field, and §28.2 keeps
// that field's reclamation at the holder's release point.
//
// This is the same static shape as row F — receiver static type `Base`,
// property declared in `Base`, arrow body, iterator-format declared head, read
// through the vtable slot; the accessor sees the same declared type and the
// same body expression. The ONLY difference is the wrap creator's verdict, and
// a claim site that cannot compute that verdict is what turned a clean program
// into a SEGV for the #6403 lane (the interface-map global called as a
// function). This row is what makes "the accessor reports its own MINT" the
// claim, rather than "the accessor reports that it is a getter".
//
// Measured on the same composed tree: clean, and STILL clean after the fix.
// ===========================================================================
BEZEL_TEST(for_in_property_read_is_a_lend_6397,
           virtual_declining_arrow_getter_claims_no_frame_spec_28_2) {
    LeakEnv scope("strict");
    JitResult r = checked(
        "module TG; " + std::string(kSelfA) +
        "public class Base "
        "  private owned _e: IEnumerable<int>; "
        "  public constructor() _e := new SelfA(3); end constructor; "
        "  public virtual property E: IEnumerable<int> get => _e; end E; "
        "end Base; "
        "public class Der: Base end Der; "
        "public class TGImpl "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<200 do "
        "      var b: Base := new Der(); "
        "      for var x in b.E do s := s + x; end; "
        "    end for; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end TGImpl;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}
