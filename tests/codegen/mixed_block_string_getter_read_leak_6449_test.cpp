// Codegen — #6449 / §23.3 "Block-form properties" + §28.9 "Runtime-managed
// types" (v0.338). A block-bodied `string` property getter whose returns are
// MIXED — at least one hands back a fresh `string` (+1 producer) and at least
// one hands back a borrow — leaked the producer on EVERY read.
//
// THE DEFECT, as MEASURED on this fix's base snapshot (the commit body names
// the pre-fix binary and the parent the reviewer reproduced against) with
// `bezelc --sanitize=address` on 200 reads:
//
//   ==NNNN==ERROR: LeakSanitizer: detected memory leaks
//   Direct leak of 6400 byte(s) in 200 object(s) allocated from:
//       #0 0x… in bz$prop$Tag$get bezel
//   Indirect leak of 1600 byte(s) in 200 object(s) allocated from:
//       #0 0x… in bz$prop$Tag$get bezel
//   SUMMARY: AddressSanitizer: 8000 byte(s) leaked in 400 allocation(s).
//
// WHY, in the two halves that had to move together.
//
//   * A block-bodied getter is emitted OUT-OF-LINE
//     (`ensurePropertyBlockGetter` → `bz$prop$<name>$get`), so every read of
//     one is a CALL. Its `return` reaches `lowerReturn` with
//     `currentReturnTypeNode_` NULL — the #2694/#2703 suppression, which is
//     correct exactly as long as both sides of the axiom-A4 lend hold: an
//     accessor's read face is +0 and no call site discharges anything.
//   * #5321 taught `isPlusOneStringProducer` to classify such a read +1, but
//     only when EVERY return is a +1 producer. Its own commit body names the
//     residue: "When not all returns are producers, the conservative borrow
//     path is used." A MIXED body classified false, so no consumer registered
//     a discharge, and the getter handed its body's value back VERBATIM — the
//     producer edge's +1 escaped to nobody.
//
// "Sometimes +1, sometimes a borrow" is not an answer that predicate can
// return, so the answer cannot be derived from the body at all: the getter has
// to NORMALIZE first, and the classification has to follow. This fix makes
// every exit of a standalone block-bodied `string` getter hand back exactly
// +1 (a retain on any return that is not already a +1 producer), and classifies
// every read of one as +1. That is the same unconditional-`+1` contract
// `ensureVirtualStringGetterOwnershipThunk` (#4952) already guarantees for a
// vtable/itable slot and `emitExportedPropertyGetters` (#5342) for a
// cross-module `$propget` stub — which is why the read face still transfers no
// NET obligation: retain inside, release at the reader. §28.9 (v0.338) is the
// authority and it settles the obligation while declining the mechanism: "A
// getter body whose result is a fresh runtime-managed production (an
// interpolation, a concatenation, a library call) remains admissible —
// runtime-managed values are exempt from the ceremony — and the implementation
// reclaims the production; **how** is not specified". The same paragraph is
// what makes the two body spellings "semantically indistinguishable at every
// read site", which is the invariant the two `arrow_bodied_*` control rows
// below pin.
//
// THE TWO HALVES ARE NOT SEPARABLE, and this file is built so that neither can
// land alone and look right:
//   * the mint WITHOUT the classification hands a consumer a +1 it treats as a
//     borrow — a permanent inflate, i.e. a leak of exactly the size above;
//   * the classification WITHOUT the mint releases a reference never minted —
//     a heap-use-after-free.
// Every pinning row below therefore runs with the LSan knob ARMED, so the
// inflate direction is caught as loudly as the leak, and the controls keep a
// getter that "simply never reclaims" from satisfying the file.
//
// ORACLE. `runAotChecked` — ALWAYS ASan, AND ALWAYS LeakSanitizer: it calls
// `runAotCheckedImpl(..., /*detectLeaks=*/true)` (aot_support.hpp:3801), so the
// child runs with `detect_leaks=1`. `runAotCheckedLenient` is the `detect_leaks=0`
// variant, and a row wanting LSan off wants THAT helper, not an env var.
//
// What a leak actually trips is `hasSanitizerReport` (aot_support.hpp:528),
// which matches the REPORT TEXT — "ERROR: AddressSanitizer", "ERROR:
// LeakSanitizer", "SUMMARY: AddressSanitizer", "detected memory leaks" — and
// not the exit code. The design note at :3717-3721 says so directly: with
// `abort_on_error=0` the sanitizer prints a report and exits 99, so "detection
// keys on the report text (M3), not the exit code". That is also why every row
// here asserts its sum on STDOUT rather than on an exit code: the 8-bit
// truncation above 255 would make an exit-code oracle unsound on its own terms,
// independent of the sanitizer.
//
// WHY `LeakEnv("strict")` IS STILL HERE even though it is not what makes these
// rows observe a leak. The `BEZEL_AOT_DETECT_LEAKS` knob is read at
// aot_support.hpp:3416 and :4014, which are `runAot` and the two-module path —
// NOT the `runAotChecked` family — so for these rows it is belt-and-braces
// rather than load-bearing. It is kept for the direction it DOES protect: the
// gate may run a whole shard with the knob set, and the RAII save/restore
// stops that from polluting a `runAot`-based row elsewhere in the same process.
// It is deliberately NOT what the leak rows rest on.
//
// Each row asserts `!checkedToolchainSkipped(r)` as its OWN statement, BEFORE
// any skip-absorbing early return, so a toolchain that cannot link the ASan
// binary fails loudly instead of being absorbed into a green row.
//
// Every program loops 200×, so a suppression or an inflate is 200 distinct
// events rather than one. All sums are asserted on STDOUT, never on an exit
// code (8-bit truncation above 255).
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
// gate may run a whole shard with the knob set, and a row that asserts knob-ON
// behaviour must not be polluted by (or pollute) its neighbours.
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

// `_label` is "abcdef" (Length 6), so every getter below takes its PRODUCER
// edge on all 200 iterations and the sums are exactly computable:
//   borrow edge  → 6 chars per read
//   producer edge → 7 chars per read ("abcdef!" / "abcdefz")
// 200 × 6 = 1200, 200 × 7 = 1400. The mixed and all-producer rows therefore
// print the SAME total, which is the point: the totals cannot tell the rows
// apart, only the leak oracle can.
const char* kPreamble =
    "class C "
    "  private _label: string; "
    "  public constructor() _label := \"abcdef\"; end constructor; ";

// 200 reads of `c.Tag`, accumulating the read value's Length. `tag` is the
// property's spelling in the driver; every row binds it to a local, which is
// the consumer position the issue's own matrix measured first.
const char* kMainBind =
    "  public static method Main(): int "
    "    var total := 0; "
    "    using var c := new C(); "
    "    for var i in 1..200 do "
    "      var s := c.TAG; "
    "      total := total + s.Length; "
    "    end for; "
    "    Console.WriteLine($\"total={total}\"); "
    "    return 0; "
    "  end Main; ";

// Same 200 reads, but consumed as a STATEMENT (the read's value is discarded
// rather than bound). The issue measured this position separately and it
// leaked identically — the discharge a consumer registers depends on the
// position, not on the body.
const char* kMainDiscard =
    "  public static method Main(): int "
    "    var total := 0; "
    "    using var c := new C(); "
    "    for var i in 1..200 do "
    "      _ := c.TAG.Length; "
    "      total := total + 1; "
    "    end for; "
    "    Console.WriteLine($\"total={total}\"); "
    "    return 0; "
    "  end Main; ";

// Same 200 reads, handed to a `string` PARAMETER — the callee borrows, so the
// argument face is where a normalized +1 has to be discharged.
const char* kMainParam =
    "  public static method Main(): int "
    "    var total := 0; "
    "    using var c := new C(); "
    "    for var i in 1..200 do "
    "      total := total + c.Take(c.TAG); "
    "    end for; "
    "    Console.WriteLine($\"total={total}\"); "
    "    return 0; "
    "  end Main; ";

// Same 200 reads, STORED into a `string` field — the store face.
const char* kMainStore =
    "  public static method Main(): int "
    "    var total := 0; "
    "    using var c := new C(); "
    "    using var d := new D(); "
    "    for var i in 1..200 do "
    "      d.store := c.TAG; "
    "      total := total + 1; "
    "    end for; "
    "    Console.WriteLine($\"total={total}\"); "
    "    return 0; "
    "  end Main; ";

// The `?.` spelling of the same read — a DIFFERENT lowering path (the
// null-conditional property arm) with its OWN classification arm in
// `isPlusOneStringProducer`, so it is a separate row rather than a variation.
const char* kMainQMark =
    "  public static method Main(): int "
    "    var total := 0; "
    "    using var c := new C(); "
    "    var q: C? := c; "
    "    for var i in 1..200 do "
    "      var s := q?.TAG; "
    "      total := total + (if s = null then 1 else s.Length); "
    "    end for; "
    "    Console.WriteLine($\"total={total}\"); "
    "    return 0; "
    "  end Main; ";

JitResult checked(const std::string& prog) { return runAotChecked(prog); }

// The same substitution for a driver used beside a HAND-WRITTEN holder (the
// arrow-bodied controls, which spell their property inline and so cannot go
// through `prog`). Exposed as a function because the substitution itself is the
// easy part to get wrong: a driver still carrying the literal `TAG` compiles to
// a program whose read site has no member `TAG` on the holder, and the row then
// fails on `r.ran` for a reason that has nothing to do with the ownership rule
// it exists to pin.
std::string driverFor(const std::string& tag) {
    std::string driver = kMainBind;
    for (std::string::size_type p = driver.find("TAG"); p != std::string::npos;
         p = driver.find("TAG", p))
        driver.replace(p, 3, tag);
    return driver;
}

// Build a whole program: the holder (with the given accessor body) plus a
// driver class running `main`. `tag` is the property's spelling and `main` the
// driver body, so one builder serves every (body × position) pair. The driver
// constants spell the read as `c.TAG`, which is substituted with `tag` here —
// each driver is written once and used against several property names.
std::string prog(const std::string& accessorDecl, const std::string& tag,
                 const std::string& main, const std::string& extraDecls = "") {
    std::string driver = main;
    for (std::string::size_type p = driver.find("TAG"); p != std::string::npos;
         p = driver.find("TAG", p))
        driver.replace(p, 3, tag);
    return "module TM; " + std::string(kPreamble) + extraDecls +
           "  public property " + tag + ": string " + accessorDecl +
           "  end " + tag + "; " +
           "  public method Take(s: string): int => s.Length; "
           "end C; "
           "class D public store: string := \"\"; end D; "
           "public class TMImpl " + driver + "end TMImpl;";
}

// The issue's LITERAL getter shape: the MIXED body this file exists for. Every
// pinning row below uses a body of this form.
const char* kMixed =
    "get => do "
    "  if _label.Length = 0 then "
    "    return _label; "
    "  else "
    "    return _label + \"!\"; "
    "  end; "
    "end; ";

}  // namespace

// ===========================================================================
// PINNING ROW — the MIXED getter, bound local. THE defect.
//
// At this fix's parent this program prints `total=1400` and then
//   SUMMARY: AddressSanitizer: 8000 byte(s) leaked in 400 allocation(s)
// with the allocation attributed to `bz$prop$Tag$get`. With the fix it prints
// the same total and exits 0 with no finding.
//
// 200 reads, all taking the producer edge (Length 6 ≠ 0): 200 × 7 = 1400.
// ===========================================================================
BEZEL_TEST(mixed_block_string_getter_read_leak_6449,
           mixed_getter_body_read_is_normalized_to_plus_one_spec_28_9) {
    LeakEnv scope("strict");
    JitResult r = checked(prog(kMixed, "Tag", kMainBind));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=1400") != std::string::npos);
}

// ===========================================================================
// CONTROL — the ALL-PRODUCER getter. LIVE, and the reason the row above is
// not satisfied by a getter that simply never reclaims.
//
// Both returns are +1 producers, so this classified +1 BEFORE the fix (#5321's
// arm) and was clean there. It is clean after it too — and that is what makes
// it a control: it pins the direction in which the change must not have moved
// anything. A getter that "fixed" the mixed leak by dropping its mint would
// make THIS row leak, because nothing releases the producer it hands back.
//
// The total is IDENTICAL to the pinning row's (both edges are producers), so
// the two rows are indistinguishable by their stdout and can only be told apart
// by the leak oracle. That is deliberate.
// ===========================================================================
BEZEL_TEST(mixed_block_string_getter_read_leak_6449,
           all_producer_getter_still_discharges_control_spec_28_9) {
    LeakEnv scope("strict");
    JitResult r = checked(prog(
        "get => do "
        "  if _label.Length = 0 then "
        "    return _label + \"z\"; "
        "  else "
        "    return _label + \"!\"; "
        "  end; "
        "end; ",
        "Tag", kMainBind));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=1400") != std::string::npos);
}

// ===========================================================================
// CONTROL — the ALL-BORROW getter. The other half of the bracket.
//
// Both returns are a field read, so nothing is produced and the +1 this fix
// adds on the borrow edge is the ONLY reference the reader ever holds. Clean
// before the fix (nothing to leak) and clean after it — but for a different
// reason: after the fix the getter retains on its way out and the reader
// releases, and a mint that the reader never discharged would show up here as
// 200 leaked strings. Together with the all-producer row this is what stops
// the pinning row from being satisfiable by "never reclaim".
//
// 200 reads, all borrow edges: 200 × 6 = 1200.
// ===========================================================================
BEZEL_TEST(mixed_block_string_getter_read_leak_6449,
           all_borrow_getter_gets_exactly_one_reference_control_spec_28_9) {
    LeakEnv scope("strict");
    JitResult r = checked(prog(
        "get => do "
        "  if _label.Length = 0 then "
        "    return _label; "
        "  else "
        "    return _label; "
        "  end; "
        "end; ",
        "Tag", kMainBind));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=1200") != std::string::npos);
}

// ===========================================================================
// PINNING ROW — the MIXED getter at the DISCARD position.
//
// The issue's matrix measured this separately and it leaked identically
// (8000 B / 400). It is a distinct row because the discharge a consumer
// registers depends on its POSITION, so a fix that only taught the bind to
// release would leave this one red.
// ===========================================================================
BEZEL_TEST(mixed_block_string_getter_read_leak_6449,
           mixed_getter_read_at_discard_position_is_normalized_spec_28_9) {
    LeakEnv scope("strict");
    JitResult r = checked(prog(kMixed, "Tag", kMainDiscard));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=200") != std::string::npos);
}

// ===========================================================================
// PINNING ROW — the MIXED getter as a `string` PARAMETER argument. The
// argument face, where the callee borrows and the caller owns the +1.
// ===========================================================================
BEZEL_TEST(mixed_block_string_getter_read_leak_6449,
           mixed_getter_read_as_string_argument_is_normalized_spec_28_9) {
    LeakEnv scope("strict");
    JitResult r = checked(prog(kMixed, "Tag", kMainParam));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=1400") != std::string::npos);
}

// ===========================================================================
// PINNING ROW — the MIXED getter STORED into a `string` field. The store face.
// ===========================================================================
BEZEL_TEST(mixed_block_string_getter_read_leak_6449,
           mixed_getter_read_stored_into_string_field_is_normalized_spec_28_9) {
    LeakEnv scope("strict");
    JitResult r = checked(prog(kMixed, "Tag", kMainStore));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=200") != std::string::npos);
}

// ===========================================================================
// PINNING ROW — the MIXED getter through `?.`.
//
// A DIFFERENT lowering path (the null-conditional property arm) with its OWN
// classification arm in `isPlusOneStringProducer`, so a fix confined to the
// plain `.` spelling would leave this red.
// ===========================================================================
BEZEL_TEST(mixed_block_string_getter_read_leak_6449,
           null_conditional_mixed_getter_read_is_normalized_spec_28_9) {
    LeakEnv scope("strict");
    JitResult r = checked(prog(kMixed, "Tag", kMainQMark));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=1400") != std::string::npos);
}

// ===========================================================================
// CONTROL — the ARROW-bodied producer. The INLINED face, which this change
// must not reach.
//
// `=> _label + "!"` (#2304's shape) is inlined at the read site rather than
// emitted out-of-line, so it is classified by the #2304 arm and normalized by
// the inlining itself. It was clean before and is clean after. It is the row
// that pins §28.9's "the two accessor body spellings … are semantically
// indistinguishable at every read site": the block-bodied mixed row above and
// this one now agree, and a change that moved the arrow face would move it.
// ===========================================================================
BEZEL_TEST(mixed_block_string_getter_read_leak_6449,
           arrow_bodied_producer_getter_is_unaffected_control_spec_23_3) {
    LeakEnv scope("strict");
    JitResult r = checked(
        "module TA; " + std::string(kPreamble) +
        "  public property Tag: string => _label + \"!\"; "
        "end C; "
        "public class TAImpl " + driverFor("Tag") + "end TAImpl;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=1400") != std::string::npos);
}

// ===========================================================================
// CONTROL — the ARROW-bodied borrow. The other spelling of the control pair,
// and the row a "+1 on every return" rule reached too far would inflate.
//
// `=> _label` hands back the receiver's own field, inlined at the read site.
// This change mints only inside `ensurePropertyBlockGetter`, so it is
// untouched; if it were not, the read would carry a +1 nobody drops and this
// row would leak 200 strings.
// ===========================================================================
BEZEL_TEST(mixed_block_string_getter_read_leak_6449,
           arrow_bodied_borrow_getter_is_unaffected_control_spec_23_3) {
    LeakEnv scope("strict");
    JitResult r = checked(
        "module TB; " + std::string(kPreamble) +
        "  public property Tag: string => _label; "
        "end C; "
        "public class TBImpl " + driverFor("Tag") + "end TBImpl;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=1200") != std::string::npos);
}

// ===========================================================================
// CONTROL — the VIRTUAL getter on the SLOT face, which this change must not
// reach.
//
// A `virtual` property read dispatches through a slot function
// (`bz$C$Tag$propget`), so it never reaches `ensurePropertyBlockGetter`'s mint
// and never goes through the plain-`.` classification arm this fix moved; its
// normalization belongs to `ensureVirtualStringGetterOwnershipThunk` (#4952),
// which retains when — and only when — its body inspection says the accessor
// is not already a producer.
//
// The body here is ALL-BORROW, so the thunk takes its wrap, the reader holds
// exactly the one reference the wrap minted, and the release balances it.
// Clean before this fix and clean after it: the row that a mint reaching the
// slot face would turn into a 200-string inflate.
//
// 200 reads, all borrow edges: 200 × 6 = 1200.
//
// NOT COVERED HERE, DELIBERATELY, AND MEASURED ON BOTH LEGS. A virtual getter
// whose body has MORE THAN ONE statement leaks on this tree either way —
// `SUMMARY: AddressSanitizer: 8000 byte(s) leaked in 400 allocation(s)`,
// attributed to `bz$C$Tag$propget`, byte-identical at this fix's parent
// (the pre-fix `/var/tmp/bezel-scratch/r31-p-6449/bezelc-PREFIX` snapshot) and with this
// fix applied, for an ALL-PRODUCER body and for the mixed body alike. The
// thunk's body inspection reads a block getter only when it is EXACTLY one
// `return <expr>;` statement, so a multi-statement body answers "unknown",
// takes the wrap, and retains on top of a +1 the accessor already transferred —
// one surplus reference per call, released by nobody.
//
// That is a SEPARATE defect on a different face with a different owner, and it
// is not what this lane's issue is about (#6449 is the STANDALONE out-of-line
// getter, `bz$prop$Tag$get`, whose allocation site is the getter body itself).
// Shipping either shape as a row here would be a permanent red on a tree this
// commit does not claim to fix, so the rows are left out and the finding is
// reported in the commit body instead.
//
// THREE READ SITES BYPASS isPlusOneStringProducer ENTIRELY, and an earlier
// draft of this header claimed every `ensurePropertyBlockGetter` caller routes
// through it. That was false. The bare-`this` read (Exprs.cpp:7727), the
// property-as-function-value thunk (Layout.cpp:8053) and the enumerator
// `Current` face (Stmts.cpp:11091) all take the getter's result without asking.
// They are covered anyway because the mint is inside the MEMOISED GETTER BODY,
// so it fires for every caller whatever the read's classification. DO NOT add a
// classification arm to one of them expecting a repair: measured over 200
// enumerator iterations, the enumerator `Current` face is byte-identical on both
// legs of this fix (mixed 8000 B/400 alloc, all-producer 8200 B/400 alloc,
// all-borrow clean), so an arm there would release a reference the face does
// not transfer — a use-after-free, not a fix. Details and numbers in the
// commit body.
// ===========================================================================
BEZEL_TEST(mixed_block_string_getter_read_leak_6449,
           virtual_all_borrow_getter_keeps_the_thunk_contract_control_spec_28_9) {
    LeakEnv scope("strict");
    JitResult r = checked(
        "module TV; " + std::string(kPreamble) +
        "  public virtual property Tag: string "
        "    get => do "
        "      if _label.Length = 0 then "
        "        return _label; "
        "      else "
        "        return _label; "
        "      end; "
        "    end; "
        "  end Tag; "
        "end C; "
        "class Der: C end Der; "
        "public class TVImpl "
        "  public static method Main(): int "
        "    var total := 0; "
        "    using var b: C := new Der(); "
        "    for var i in 1..200 do "
        "      var s := b.Tag; "
        "      total := total + s.Length; "
        "    end for; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end TVImpl;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("total=1200") != std::string::npos);
}