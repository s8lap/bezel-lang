// fix #3857 — §15.4.1's user-defined sequence-literal protocol borrowing
// carrier changed from `ReadOnlySpan<T>` to `ReadOnlyArraySpan<T>` at spec
// v0.272 (proposal 0115, `docs/Bezel_Changelog.md` v0.272 entry). The
// compiler's recognition (`Resolver::checkCollectionLiteralFactoryConjuncts`,
// TypeResolution_Checks.cpp; `Emitter::materializeUserTypeCollectionLiteral`,
// LlvmBackend_Setup.cpp) was never updated: a factory written to the CURRENT
// spec — `public static method FromCollectionLiteral(elements:
// ReadOnlyArraySpan<T>): owned C<T>` — was silently treated as a non-opt-in
// (`isCollectionLiteralAdmissibleTarget`, ExprTypecheck_Calls.cpp, admits ANY
// static `FromCollectionLiteral` by NAME alone, so the literal was accepted
// at sema), and codegen's `materializeUserTypeCollectionLiteral` returned
// null for the unrecognized carrier, falling through to the inline-array GEP
// path and tripping the LLVM verifier ("Invalid indices for GEP pointer
// type!") — measured on a clean `6f46a73` build, `new_carrier_...` below.
//
// THE OLD SPELLING. `ReadOnlySpan<T>` is no longer part of the protocol —
// proposal 0115's D1 explains why: it named the OWNING family's view
// (backed by `OwningBuffer<T>`, which owns its class-T slots) for a protocol
// whose elements are borrows, an ownership incoherence rather than a mere
// rename. Rather than leave it silently unrecognized (the exact defect this
// issue reports), the fix rejects it at the declaration with a remedy
// naming the current carrier — `old_carrier_readonlyspan_rejected_*` below.
//
// D4 — THE SOUNDNESS COMPANION, MANDATORY PER THE SAME CHANGELOG ENTRY. The
// v0.272 entry states plainly: "Landing D1 alone would convert an ill-typed
// program into a well-typed leak — strictly worse than the defect being
// fixed — so D1 does not land without D4." Before D1, a class-typed-T
// borrowing-form literal never reached the ownership question at all: the
// factory BODY itself failed to type-check under the old carrier (a `T?`
// indexer read against a non-nullable `T` sink, §15.4.1's own reported
// symptom, #3310). Landing D1 alone makes that surface newly reachable —
// `RBag<Conn> := [new Conn(1)]` would newly COMPILE and leak, because
// `checkBorrowingFamilyLiteralElements` (§15.4's fresh-producer rejection,
// ExprTypecheck.cpp) excluded every user type unconditionally. D4 extends
// that check's target recognition to a user borrowing-form target — "the
// existing §15.4 predicate applied to one more target class, not a parallel
// rule" (the changelog's own kernel classification) —
// `fresh_producer_of_nontrivial_dispose_class_rejected_*` below is the row
// that would leak without it.
//
// per §15.4, §15.4.1, §28.3, §28.9 (issue #3857, spec v0.272)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// `Conn` — non-trivial-dispose by destructor (§28.9's first disqualifier),
// mirrors the idiom `tests/sema/borrowing_family_literal_fresh_producer_2673_test.cpp`
// established for the SAME question at builtin borrowing-family targets.
// `Box` — trivial-dispose (no destructor, no `owned` field): the accept-side
// control for §15.4's next paragraph ("a fresh element of trivial-dispose
// class type is admitted").
const char* kPrelude =
    "module M; "
    "class Conn(id: int) "
    "  public Id: int := id; "
    "  destructor end destructor; "
    "end Conn; "
    "class Box(v: int) "
    "  public V: int := v; "
    "end Box; "
    // Non-generic, class-typed-T borrowing-form factory. Reads `elements[i].Id`
    // directly (no `?.`/`!`) — this is the exact shape that fails to
    // type-check under the OLD `ReadOnlySpan<T>` carrier per #3310 (a `T?`
    // indexer at class-typed T), and the exact shape §4.15's category-uniform
    // `public property [i: int]: T` indexer (`Bezel_Standard_Library.md`)
    // makes admissible under the new one.
    "class RConnBag "
    "  public Total: int; "
    "  public static method FromCollectionLiteral("
    "      elements: ReadOnlyArraySpan<Conn>): owned RConnBag "
    "    var t: int := 0; "
    "    for var i in 0..<elements.Length do "
    "      t := t + elements[i].Id; "
    "    end; "
    "    return new RConnBag(t); "
    "  end FromCollectionLiteral; "
    "  public constructor(n: int) Total := n; end constructor; "
    "end RConnBag; "
    "class RBoxBag "
    "  public Total: int; "
    "  public static method FromCollectionLiteral("
    "      elements: ReadOnlyArraySpan<Box>): owned RBoxBag "
    "    var t: int := 0; "
    "    for var i in 0..<elements.Length do "
    "      t := t + elements[i].V; "
    "    end; "
    "    return new RBoxBag(t); "
    "  end FromCollectionLiteral; "
    "  public constructor(n: int) Total := n; end constructor; "
    "end RBoxBag; "
    // Value-typed-T factory — proves the NEW carrier is recognized end to
    // end (sema admits it, codegen materializes it, native exec produces
    // the right value) independent of the class-typed / D4 question.
    "class VBag "
    "  public Sum: int; "
    "  public static method FromCollectionLiteral("
    "      elements: ReadOnlyArraySpan<int>): owned VBag "
    "    var t: int := 0; "
    "    for var i in 0..<elements.Length do "
    "      t := t + elements[i]; "
    "    end; "
    "    return new VBag(t); "
    "  end FromCollectionLiteral; "
    "  public constructor(n: int) Sum := n; end constructor; "
    "end VBag; ";

std::string prog(const std::string& body) {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int " +
           body +
           "  end Main; "
           "end C;";
}

// True iff SOME diagnostic carries the OLD-carrier-rejected message this fix
// adds (`Resolver::checkCollectionLiteralFactoryConjuncts`).
bool rejectedOldCarrier(const JitResult& r, const std::string& typeName) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("`FromCollectionLiteral` on type `" + typeName +
                            "` declares its borrowing carrier as "
                            "`ReadOnlySpan<T>`") != std::string::npos &&
            d.message.find("no longer recognized") != std::string::npos &&
            d.message.find("ReadOnlyArraySpan<T>") != std::string::npos &&
            d.message.find("v0.272") != std::string::npos &&
            d.message.find("§15.4.1") != std::string::npos)
            return true;
    }
    return false;
}

// True iff SOME diagnostic carries §15.4's borrowing-family fresh-producer
// rejection (same predicate shape as the 2673 test — the rule is the same
// rule, only the target class of #3857/D4 is new).
bool rejectedBorrowingFamilyFreshProducer(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(
                "is a fresh producer of the non-trivial-dispose class type") !=
                std::string::npos &&
            d.message.find("does not take ownership") != std::string::npos &&
            d.message.find("§15.4") != std::string::npos)
            return true;
    return false;
}

bool offersOwningFamilyRemedy(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("use the owning family") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// =====================================================================
// PART 1 — the new carrier is recognized end to end (sema + codegen +
// native exec), value-typed T. Pre-fix this is the issue's IR crash
// ("Invalid indices for GEP pointer type!"), not a value mismatch — the
// value-verify is the non-vacuous accept per LANE_RULES: "it compiled" is
// the whole bug this fix closes.
// =====================================================================

BEZEL_TEST(sema_collection_literal_borrow_form_readonlyarrayspan_3857,
           new_carrier_readonlyarrayspan_recognized_value_typed_spec_15_4_1) {
    JitResult r = runAot(
        prog("    var v: VBag := [10, 20, 30]; "
             "    return v.Sum; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// =====================================================================
// PART 2 — the new carrier works at CLASS-typed T via a named borrow, the
// "newly accepted" surface D1 exists for: §15.4.1's borrowing form did not
// type-check at class-typed T at all under the old carrier (#3310's T?
// indexer symptom). `elements[i].Id` (plain member access, no `?.`) only
// type-checks if the indexer's static result is non-nullable `Conn` —
// exactly what §4.15's category-uniform indexer gives the new carrier.
// =====================================================================

BEZEL_TEST(sema_collection_literal_borrow_form_readonlyarrayspan_3857,
           new_carrier_works_at_class_typed_element_via_named_borrow_spec_15_4_1) {
    JitResult r = runAot(
        prog("    using var c1 := new Conn(10); "
             "    using var c2 := new Conn(20); "
             "    using var c3 := new Conn(30); "
             "    var rc: RConnBag := [c1, c2, c3]; "
             "    return rc.Total; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// =====================================================================
// PART 3 — the OLD `ReadOnlySpan<T>` spelling is no longer recognized: it
// is rejected at the declaration with a remedy naming the current carrier,
// rather than silently falling through to the unrecognized-signature path
// (which is what let #3857's defect hide — the literal was ADMITTED at
// sema on name alone and crashed codegen).
// =====================================================================

BEZEL_TEST(sema_collection_literal_borrow_form_readonlyarrayspan_3857,
           old_carrier_readonlyspan_rejected_spec_15_4_1_v0_272) {
    JitResult r = runAot(
        "module M; "
        "class OldCarrier "
        "  public static method FromCollectionLiteral("
        "      elements: ReadOnlySpan<int>): owned OldCarrier "
        "    return new OldCarrier(); "
        "  end FromCollectionLiteral; "
        "end OldCarrier; "
        "class C "
        "  public static method Main(): int "
        "    var x: OldCarrier := [1]; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedOldCarrier(r, "OldCarrier"));
}

// =====================================================================
// PART 4 (D4) — REJECT: a fresh producer of a non-trivial-dispose class at
// a user BORROWING-form target is rejected at the literal site, exactly as
// it is at a builtin borrowing-family target (#2673). Without this, D1
// alone would let this program newly COMPILE and leak (`Conn`'s destructor
// never runs) — the changelog's own stated reason D1 does not land without
// D4.
// =====================================================================

BEZEL_TEST(sema_collection_literal_borrow_form_readonlyarrayspan_3857,
           fresh_producer_of_nontrivial_dispose_class_rejected_at_user_borrow_target_d4) {
    JitResult r = runAot(
        prog("    var rc: RConnBag := [new Conn(1), new Conn(2)]; "
             "    return 0; "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowingFamilyFreshProducer(r));
    // A user borrow-form target has no built-in "Owning<Head>" alternative
    // to name — §15.4.1 declares exactly one form per type — so the
    // owning-family half of the builtin diagnostic's remedy must not print.
    BEZEL_EXPECT_FALSE(offersOwningFamilyRemedy(r));
}

// =====================================================================
// PART 5 (D4 accept-side control, LANE_RULES §15) — a fresh TRIVIAL-dispose
// element stays admissible at a user borrow-form target, exactly as §15.4's
// next paragraph keeps it admissible at a builtin one: "a fresh element of
// trivial-dispose class type is admitted … as a fresh node of the literal
// tree." Pins that D4's new target recognition did not over-reject the
// neighbour it must not touch.
// =====================================================================

BEZEL_TEST(sema_collection_literal_borrow_form_readonlyarrayspan_3857,
           fresh_trivial_dispose_element_admitted_at_user_borrow_target_d4) {
    JitResult r = runAot(
        prog("    var rb: RBoxBag := [new Box(1), new Box(2), new Box(3)]; "
             "    return rb.Total; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}
