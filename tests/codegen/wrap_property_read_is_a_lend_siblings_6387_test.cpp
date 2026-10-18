// Codegen — the #6387 SIBLING SITES of the ownsSrc producer, §28.9 + §28.6 +
// §28.2 (v0.338).
//
// `narrow #6387` (picked as 0f8306bea5) corrected the producer of a bad
// `ownsSrc` flag at ONE of the sites where
// `sema::isAutoPropertyStorageSymbol` is spelled out as "does this property
// have compiler-generated backing storage?". That predicate's NO answer was
// read as "this read produces a FRESH value the reader may own". For a
// hand-written getter those are two different questions, so the flag came back
// `true` and the wrapper freed a field its holder still owns.
//
// §28.9 (v0.338) settles it in one sentence: "a property or indexer READ is a
// lend for every declared type … a getter never transfers a reclamation
// obligation to its reader." §28.2 (v0.338) completes it — "Every class
// instance is reclaimed by compiler-inserted code at its holder's release
// point … and nowhere else."
//
// The predicate is written out at NINE sites across four codegen files, not
// four. #6387 changed one. This file pins the TWO more that were MEASURED to
// carry the same defect and are fixed alongside it:
//
//   * src/codegen/LlvmBackend_Setup.cpp — the local-decl coercion producer,
//     `var e: IEnumerable<int> := h.Seq;`. The identical
//     `/*ownsSrc=*/!srcIsNamedOwner` handoff to
//     `wrapUserClassAsIEnumerable`, from an initializer instead of a
//     `.GetEnumerator()` receiver.
//   * src/codegen/LlvmBackend_Calls2.cpp — the `.GetEnumerator()` receiver
//     producer, `h.Seq.GetEnumerator()`. The identical handoff, from the call's
//     receiver instead of a local-decl initializer.
//
// THE DISCRIMINATOR IS THE `PropertySymbol` ARM, NOT THE PROGRAM SHAPE. Each
// pinning row below is the defect spelling; its control row is the SAME program
// with the receiver spelling changed from a computed property to a FIELD, which
// takes the `FieldSymbol` arm and never consults the predicate. MEASURED at
// this file's parent (`r30-c1` @ 82673c16), under
// `build/bezelc --sanitize=address`:
//
//   property receiver, both spellings  heap-use-after-free, READ of size 8,
//                                     in `Holder$DisposeCore`, freed by
//                                     `bz$userenumenum$SelfA$SelfA$Dispose`
//                                     (the wrapper reclaiming the holder's
//                                     field), allocated in `Holder$ctor`
//   field receiver, both spellings    CLEAN, prints `s=1200`
//
// So a fix that reached past the `PropertySymbol` arm — into the
// Local/Parameter/Field classification, or into the `wrapUserClassAsIEnumerable`
// call itself — would move a control row, and nothing else in this file would
// notice. That is what the two control rows are for.
//
// WHAT THE WIDENING GIVES UP: NOTHING, AND THE ROWS BELOW PROVE IT. Both fixed
// arms are entered only when the value's static type is a USER CLASS, and sema
// REJECTS a getter returning a freshly-produced owned value of exactly that
// shape — "a property or indexer getter may not return a freshly-produced owned
// value — a getter is a borrow-producing position (properties and indexers do
// not carry `owned`), so the value would escape with no owner and leak; expose
// the factory as a method returning `owned T`" (§28.6). MEASURED on this
// file's parent: `get => Fresh();` where `Fresh(): SelfA return new SelfA(3);`
// does not compile. The rejection is pinned by its own row below, so a future
// change that RELAXED it (letting a getter hand back a fresh owned class) would
// turn this fix into a leak and would be caught here. An auto-property is a
// `PropertySymbol` too, so the auto-property sub-case is subsumed rather than
// narrowed: its answer does not change.
//
// ORACLE. `runAotChecked`, which is ALWAYS ASan on this box, nonzero exit on
// ANY finding. The PLAIN leg is UNRELIABLE for this defect class — the free and
// the later read both land on the same 24-byte block, so a non-instrumented run
// can print the right answer and exit 0. Each row loops 200× so the freed block
// is REUSED rather than merely faulting once; the `s=1200` assertion is what
// proves the loop body actually ran, so a row cannot be vacuously clean.
//
// NO ROW HERE ASSERTS THAT A LEAK OR A USE-AFTER-FREE IS PRESENT. Every row
// asserts the program runs clean, which is what stops the file inverting the
// moment the defect is fixed.
//
// SPELLING COVERAGE, STATED HONESTLY. All four runnable rows exercise the SAME
// family of class-typed computed getters (`get => _a;` — the expression-bodied
// single-return form). The BLOCK-bodied getter form (`get => do return _a; end`)
// and the `IEnumerable<T>`-declared property form are NOT exercised here; the
// #6387 file (`wrap_property_read_is_a_lend_6387_test.cpp`) covers the
// block-bodied form. A `cpp_only` mutant of the widening would therefore not be
// seen by this file alone — the #6387 rows are the other half.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// From #6382's `kSelfA` and #6387's, verbatim: it DECLARES the enumerator
// protocol itself, so `wrapUserClassAsIEnumerable`'s structural scan picks
// `enumTs == srcTs` and the wrapper's slot-1 disposer resolves.
const char* kSelfA =
    "public class SelfA: IEnumerable<int>, IEnumerator<int> "
    "  private cur: int := 0; private hi: int; "
    "  public constructor(hi: int) this.hi := hi; end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> return this; end GetEnumerator; "
    "  public method MoveNext(): bool cur := cur + 1; return cur <= hi; end MoveNext; "
    "  public property Current: int get => cur; end Current; "
    "end SelfA; ";

// The defect spelling: a HAND-WRITTEN getter over the holder's `owned` field.
// `isAutoPropertyStorageSymbol` answers false here ("no compiler-generated
// backing storage"), which is a true answer to a question nobody asked.
const char* kComputedProperty =
    "  private owned _a: SelfA; "
    "  public constructor() _a := new SelfA(3); end constructor; "
    "  public property Seq: SelfA get => _a; end Seq; ";

// The control spelling: the receiver IS the field. `FieldSymbol` answers
// `srcIsNamedOwner = true` on both sides of the fix.
const char* kField =
    "  public owned A: SelfA := new SelfA(3); ";

JitResult checked(const std::string& prog) { return runAotChecked(prog); }

}  // namespace

// ===========================================================================
// SITE 1 — src/codegen/LlvmBackend_Setup.cpp, the local-decl coercion
// producer. `var e: IEnumerable<int> := h.Seq;` mints the §29.2 wrapper with
// `/*ownsSrc=*/!srcIsNamedOwner`; the wrapper then reclaims `Holder._a` at the
// end of the `for`, and `Holder$DisposeCore` reads the freed field.
// ===========================================================================
BEZEL_TEST(wrap_property_read_is_a_lend_siblings_6387,
           local_decl_binding_of_computed_property_read_must_not_reclaim) {
    JitResult r = checked("module T1; " + std::string(kSelfA) +
                          "public class Holder " + kComputedProperty +
                          "end Holder; "
                          "public class T1Impl "
                          "  public static method Main(): int "
                          "    var s := 0; "
                          "    for var i in 0..<200 do "
                          "      var h := new Holder(); "
                          "      var e: IEnumerable<int> := h.Seq; "
                          "      for var x in e do s := s + x; end; "
                          "    end for; "
                          "    Console.WriteLine($\"s={s}\"); "
                          "    return 0; "
                          "  end Main; "
                          "end T1Impl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// CONTROL for site 1 — the same wrap, the same arm, the same holder, the
// receiver spelled as the FIELD instead of a property read. GREEN on both sides
// of the fix by construction.
BEZEL_TEST(wrap_property_read_is_a_lend_siblings_6387,
           local_decl_binding_of_field_read_control_stays_green) {
    JitResult r = checked("module T1C; " + std::string(kSelfA) +
                          "public class Holder " + kField +
                          "end Holder; "
                          "public class T1CImpl "
                          "  public static method Main(): int "
                          "    var s := 0; "
                          "    for var i in 0..<200 do "
                          "      var h := new Holder(); "
                          "      var e: IEnumerable<int> := h.A; "
                          "      for var x in e do s := s + x; end; "
                          "    end for; "
                          "    Console.WriteLine($\"s={s}\"); "
                          "    return 0; "
                          "  end Main; "
                          "end T1CImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// ===========================================================================
// SITE 2 — src/codegen/LlvmBackend_Calls2.cpp, the `.GetEnumerator()` receiver
// producer. Same handoff, same free site, same discriminator.
// ===========================================================================
BEZEL_TEST(wrap_property_read_is_a_lend_siblings_6387,
           get_enumerator_receiver_computed_property_must_not_reclaim) {
    JitResult r = checked("module T2; " + std::string(kSelfA) +
                          "public class Holder " + kComputedProperty +
                          "end Holder; "
                          "public class T2Impl "
                          "  public static method Main(): int "
                          "    var s := 0; "
                          "    for var i in 0..<200 do "
                          "      var h := new Holder(); "
                          "      using var e := h.Seq.GetEnumerator(); "
                          "      while e.MoveNext() do s := s + e.Current; end while; "
                          "    end for; "
                          "    Console.WriteLine($\"s={s}\"); "
                          "    return 0; "
                          "  end Main; "
                          "end T2Impl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// CONTROL for site 2 — the field-receiver twin.
BEZEL_TEST(wrap_property_read_is_a_lend_siblings_6387,
           get_enumerator_receiver_field_control_stays_green) {
    JitResult r = checked("module T2C; " + std::string(kSelfA) +
                          "public class Holder " + kField +
                          "end Holder; "
                          "public class T2CImpl "
                          "  public static method Main(): int "
                          "    var s := 0; "
                          "    for var i in 0..<200 do "
                          "      var h := new Holder(); "
                          "      using var e := h.A.GetEnumerator(); "
                          "      while e.MoveNext() do s := s + e.Current; end while; "
                          "    end for; "
                          "    Console.WriteLine($\"s={s}\"); "
                          "    return 0; "
                          "  end Main; "
                          "end T2CImpl;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("s=1200") != std::string::npos);
}

// ===========================================================================
// THE PREMISE ROW — what the two widening edits above give up.
//
// Both are reached only when the value's static type is a USER CLASS, and a
// getter returning a freshly-produced owned value of that shape is REJECTED
// (the "borrow-producing position" rule). This row pins the rejection, so a
// future RELAXATION of it — which would turn the widening into a leak, because
// `isAutoPropertyStorageSymbol`'s `false` would once again have been the right
// answer — is caught here rather than shipped.
//
// Asserted as "the program does not compile", not as a diagnostic substring:
// a needle on the message text would make this row a tripwire on any rewording
// of a diagnostic that has nothing to do with the behaviour being pinned. The
// message this row rejects with, verbatim, is
//   "a property or indexer getter may not return a freshly-produced owned
//    value — a getter is a borrow-producing position (properties and indexers
//    do not carry `owned`), so the value would escape with no owner and leak;
//    expose the factory as a method returning `owned T`".
// ===========================================================================
BEZEL_TEST(wrap_property_read_is_a_lend_siblings_6387,
           computed_property_of_user_class_still_rejects_a_fresh_owned_value) {
    JitResult r = checked(
        "module TF; " + std::string(kSelfA) +
        "public class Holder "
        "  public method Fresh(): SelfA return new SelfA(3); end Fresh; "
        "  public property Seq: SelfA get => Fresh(); end Seq; "
        "end Holder; "
        "public class TFImpl "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    var e: IEnumerable<int> := h.Seq; "
        "    for var x in e do end; "
        "    return 0; "
        "  end Main; "
        "end TFImpl;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(!r.ran);
}