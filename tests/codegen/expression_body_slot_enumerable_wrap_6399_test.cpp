// Codegen — #6399 / §29.2 (v0.338).
//
// THE DEFECT. §29.2 (v0.338) desugars `for var x in source` over an
// `IEnumerable<T>`-typed source into a `GetEnumerator()` / `MoveNext()` /
// `Current` dispatch through the iterator vtable, so the value the source
// expression produces must be a real vtable-backed frame. For a property whose
// declared type is a pure `IEnumerable<T>` and whose GETTER BODY yields a raw
// collection or user-class descriptor, the compiler mints that frame with a
// §29.2 wrapper — but it minted it on only TWO of the getter's three faces:
//
//   * the INLINED read (`wrapPropertyArrayAsIEnumerable` at the property read
//     site, the #2815 call) — WRAPPED. This is the only face a CONCRETE
//     receiver takes;
//   * the BLOCK-bodied getter's RETURN edge (`wrapAsIEnumerableForTarget` in
//     `emitReturn`, #2703) — WRAPPED;
//   * the EXPRESSION-BODIED getter read through a slot function (vtable /
//     itable accessor) — NOT WRAPPED, because an arrow body has no RETURN
//     edge and the only wrap on that face was the inlined-read one, which a
//     dispatching read never takes.
//
// A virtual or interface read is exactly the face that needs the slot
// function, so the raw backing pointer escaped to a caller that drove §29.2
// vtable dispatch on it.
//
// MEASURED at this file's parent, `build/bezelc --sanitize=address`, the
// issue's own program:
//
//   ==NNNN==ERROR: AddressSanitizer: SEGV on unknown address 0x000000000010
//   (pc 0x0000003644f3 …)
//   … The signal is caused by a READ memory access.
//
// — a read of offset 0x10 of a NULL receiver, which is `SelfA`'s `hi` field.
// With the fix the same program prints `s=1200` and exits 0 with no finding.
//
// KIND x SPELLING x POSITION, enumerated rather than assumed:
//   source family  — user-class `IEnumerable<T>` implementer (row 1) AND
//                    `Array<int>` (row 2). Both SEGV'd at the parent; the
//                    array spelling faults at `pc 0x0` (a literal NULL
//                    call), the user-class spelling at `pc` inside `main`
//                    after inlining, reading offset 0x10.
//   reader spelling — `var e: IEnumerable<int> := d.E;` + `for x in e`
//                    (rows 1-2) AND a bare `for x in d.E` (row 3). Both
//                    SEGV'd at the parent.
//   dispatch face  — ITABLE, interface-typed receiver (rows 1-3) AND
//                    VTABLE, `virtual` property read through a base-typed
//                    receiver (row 6). Both SEGV'd at the parent; both are
//                    the same `ensurePropertyAccessorFn` expression-bodied
//                    arm, because one accessor function serves both slot
//                    kinds.
//   body form      — arrow (every row above) vs BLOCK (rows 4-5, controls
//                    that are green on BOTH sides, which is what proves the
//                    fix did not disturb the RETURN-edge wrap).
//
// ORACLE. Rows 1-5 use `runAotChecked`, which is ALWAYS ASan and exits
// nonzero on any finding. Row 6 uses the plain `runAot` for a reason stated
// at the row: it pins the VALUE, and its frame reclamation is a measured open
// residual in a file this lane does not own.
//
// NO ROW HERE ASSERTS THAT A CRASH OR A LEAK IS PRESENT. The defect is the
// absence of a finding: each row asserts the program RUNS CLEAN and prints
// the right answer, which is what stops inverting the moment the defect is
// fixed.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The user-class `IEnumerable<T>` implementer, verbatim from issue #6399's
// repro (and from #6382's / #6387's `kSelfA`): it DECLARES the enumerator
// protocol itself, so `wrapUserClassAsIEnumerable`'s structural scan picks
// `enumTs == srcTs` and the wrapper's slot-1 disposer resolves.
const char* kSelfA =
    "public class SelfA: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; "
    "    end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; "
    "    end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end SelfA; ";

// The interface whose body-less `readonly property E: IEnumerable<int>;` is
// the read the issue is about: at an interface-typed receiver sema resolves
// `d.E` to THIS declaration, which has no accessor body.
const char* kISeq =
    "public interface ISeq "
    "  readonly property E: IEnumerable<int>; "
    "end ISeq; ";

// `kArrowImpl` / `kBlockImpl` — the same class, the same `owned` field, the
// same declared property type, differing ONLY in the getter's body form.
const char* kArrowImpl =
    "public class SeqImpl: ISeq "
    "  private owned _e: SelfA; "
    "  public constructor() _e := new SelfA(3); end constructor; "
    "  public property E: IEnumerable<int> get => _e; end E; "
    "end SeqImpl; ";

const char* kBlockImpl =
    "public class SeqImpl: ISeq "
    "  private owned _e: SelfA; "
    "  public constructor() _e := new SelfA(3); end constructor; "
    "  public property E: IEnumerable<int> "
    "    get => do "
    "      return _e; "
    "    end; "
    "  end E; "
    "end SeqImpl; ";

// The ARRAY-backed twin of the source family: same reader, same declared
// property type, a different §29.2 wrap helper and a different backing
// representation. At the parent this spelling faults at `pc 0x0` — a literal
// NULL call — rather than the user-class spelling's field read.
const char* kArrayImpl =
    "public class SeqImpl: ISeq "
    "  private owned _e: Array<int>; "
    "  public constructor() "
    "    _e := new Array<int>(); _e.Add(1); _e.Add(2); _e.Add(3); "
    "  end constructor; "
    "  public property E: IEnumerable<int> get => _e; end E; "
    "end SeqImpl; ";

// Issue #6399's repro body, verbatim.
const char* kMain =
    "  public static method Main(): int "
    "    var s := 0; "
    "    for var i in 0..<200 do "
    "      var q := new SeqImpl(); "
    "      var d: ISeq := q; "
    "      var e: IEnumerable<int> := d.E; "
    "      for var x in e do s := s + x; end; "
    "    end for; "
    "    Console.WriteLine($\"s={s}\"); "
    "    return 0; "
    "  end Main; ";

// The reader spelling that binds nothing: a bare `for x in d.E`.
const char* kMainDirectFor =
    "  public static method Main(): int "
    "    var s := 0; "
    "    for var i in 0..<200 do "
    "      var q := new SeqImpl(); "
    "      var d: ISeq := q; "
    "      for var x in d.E do s := s + x; end; "
    "    end for; "
    "    Console.WriteLine($\"s={s}\"); "
    "    return 0; "
    "  end Main; ";

// The concrete-receiver control. Identical to row 1 except that `d` is
// `SeqImpl`, so the read is INLINED at the use site and never builds a slot
// function. GREEN at the parent and green with the fix: it is here because a
// fix that reached into `wrapPropertyArrayAsIEnumerable` itself (rather than
// adding a call to the accessor function) would move it, and no other row in
// this file would notice.
const char* kMainConcrete =
    "  public static method Main(): int "
    "    var s := 0; "
    "    for var i in 0..<200 do "
    "      var q := new SeqImpl(); "
    "      var e: IEnumerable<int> := q.E; "
    "      for var x in e do s := s + x; end; "
    "    end for; "
    "    Console.WriteLine($\"s={s}\"); "
    "    return 0; "
    "  end Main; ";

JitResult checked(const std::string& prog) { return runAotChecked(prog); }

}  // namespace

// ===========================================================================
// THE PINNING ROW — the issue's literal repro. MEASURED RED at this file's
// parent with the SEGV signature quoted in the file header.
// ===========================================================================
BEZEL_TEST(expression_body_slot_enumerable_wrap_6399,
           interface_dispatched_arrow_property_wraps_user_class_source) {
    JitResult r = checked("module T1; " + std::string(kSelfA) +
                          std::string(kISeq) + kArrowImpl +
                          "public class M1 " + kMain + "end M1;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 × (1 + 2 + 3) — the loops genuinely ran.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// The SECOND SOURCE SPELLING of the same arm: an `Array<int>` body instead of
// a user-class implementer. A different §29.2 wrap helper, a different backing
// representation, and a different fault address (`pc 0x0` rather than a field
// read of a null receiver) at the parent. Green on both sides would mean the
// row is not exercising the same thing row 1 is.
BEZEL_TEST(expression_body_slot_enumerable_wrap_6399,
           interface_dispatched_arrow_property_wraps_array_source) {
    JitResult r = checked("module T2; " + std::string(kISeq) + kArrayImpl +
                          "public class M2 " + kMain + "end M2;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// The SECOND READER SPELLING of the same arm: no intermediate local, the
// for-in's source IS the interface-dispatched property read. The reader-side
// reclamation for this spelling is a different ladder rung than the local
// declaration's, so it is a genuinely distinct path through the fix.
BEZEL_TEST(expression_body_slot_enumerable_wrap_6399,
           interface_dispatched_arrow_property_wraps_under_a_bare_for_in) {
    JitResult r = checked("module T3; " + std::string(kSelfA) +
                          std::string(kISeq) + kArrowImpl +
                          "public class M3 " + kMainDirectFor + "end M3;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// CONTROL — the BLOCK-bodied twin of row 1, same interface, same field, same
// reader. Its wrap is minted on the RETURN edge inside the accessor and was
// already correct at the parent, so this row is GREEN on both sides. It is
// here because the fix adds a wrap one arm away from it, and because it is
// what makes "the fix made the arrow face agree with the block face" the
// claim this file makes rather than "the fix made a crash go away".
BEZEL_TEST(expression_body_slot_enumerable_wrap_6399,
           interface_dispatched_block_property_stays_green) {
    JitResult r = checked("module T4; " + std::string(kSelfA) +
                          std::string(kISeq) + kBlockImpl +
                          "public class M4 " + kMain + "end M4;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// CONTROL — the CONCRETE-receiver twin of row 1, whose read is inlined at the
// use site and so never builds an accessor function. See the note on
// `kMainConcrete`.
BEZEL_TEST(expression_body_slot_enumerable_wrap_6399,
           concrete_receiver_arrow_property_stays_green) {
    JitResult r = checked("module T5; " + std::string(kSelfA) +
                          std::string(kISeq) + kArrowImpl +
                          "public class M5 " + kMainConcrete + "end M5;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// The VTABLE dispatch face — `virtual` property, read through a BASE-typed
// receiver, so the read takes the vtable slot rather than the itable slot. It
// is the same `ensurePropertyAccessorFn` expression-bodied arm as rows 1-3,
// because one accessor function serves both slot kinds, and it SEGMENTED at
// this file's parent (ASan: `SEGV on unknown address 0x000000000000
// (pc 0x000000000000)`).
//
// WHY THIS ROW IS NOT ON THE ASAN ORACLE, stated plainly rather than left for
// a reader to discover. The fix makes this program produce the RIGHT ANSWER;
// what it does not yet do is give the reader a reclamation obligation for the
// frame the accessor mints. MEASURED with `LSAN_OPTIONS=use_stacks=0` (LSan's
// conservative stack scan otherwise reports a still-live slot's frame as
// reachable, which is how the concrete-receiver control reads clean while
// holding the same frame):
//
//   Direct leak of 6400 byte(s) in 200 object(s) allocated from:
//       #0 … in calloc
//       #1 … in bz$Base$E$propget
//
// 6400 / 200 = 32 bytes = exactly one `userenumenum` wrapper frame per read.
// The reclamation gap is a READER-SIDE one and its site is NOT this file: the
// `ownsSource` ladder in `registerIterableSourceDisposal`
// (`src/codegen/LlvmBackend_Setup.cpp`) claims the frame through
// `blockGetterOf` for a BLOCK-bodied getter on ANY dispatch face, and through
// `isInterfaceDispatchedIteratorRead` for an INTERFACE-dispatched read of
// either body form — which is why rows 1-5 are clean — but has no rung for a
// VTABLE-dispatched read of an ARROW-bodied getter, so nothing claims this
// one. Adding that rung is a `LlvmBackend_Setup.cpp` edit, which this lane
// does not own and which another lane holds a live claim on.
//
// So this row pins the VALUE (the SEGV must not come back) on the plain
// oracle, and asserts nothing about the leak in either direction: asserting it
// would invert the moment the Setup.cpp rung lands. The residual is carried in
// the commit body.
BEZEL_TEST(expression_body_slot_enumerable_wrap_6399,
           virtual_dispatched_arrow_property_yields_the_right_value) {
    JitResult r = runAot(
        "module T6; " + std::string(kSelfA) +
        "public class Base "
        "  private owned _e: SelfA; "
        "  public constructor() _e := new SelfA(3); end constructor; "
        "  public virtual property E: IEnumerable<int> get => _e; end E; "
        "end Base; "
        "public class Der: Base end Der; "
        "public class M6 "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<200 do "
        "      var b: Base := new Der(); "
        "      var e: IEnumerable<int> := b.E; "
        "      for var x in e do s := s + x; end; "
        "    end for; "
        "    Console.WriteLine($\"s={s}\"); "
        "    return 0; "
        "  end Main; "
        "end M6;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}