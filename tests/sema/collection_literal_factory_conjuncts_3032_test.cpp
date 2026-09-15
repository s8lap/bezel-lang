// fix #3032 — §15.4.1 (v0.262) requires a static `FromCollectionLiteral`
// factory on a user type to be `public` and to return `owned C<T>` at the
// enclosing type's own arity, and forbids declaring both the
// `ReadOnlyArraySpan<T>` and `OwningArray<T>` carriers on the same type. #2957
//
// narrow #3857 (test-only, §15.4.1, v0.272): the borrowing carrier every
// cell below spells was renamed `ReadOnlySpan<T>` → `ReadOnlyArraySpan<T>`
// by proposal 0115; the old spelling is no longer recognized at all (rejected
// at the declaration, `TypeResolution_Checks.cpp`), so a cell still spelling
// it would be rejected at that new check instead of exercising the conjunct
// the cell is named for. Updated in the same commit as the recognition fix.
//
// deliberately scoped the recognition predicate to the three
// mechanically-distinguishable conjuncts (exact name, `static`, exactly one
// recognized carrier) and declared these three conjuncts out of scope
// (the deliberate scope boundary this fix closes). Pre-fix the program
// silently accepted a non-`public` factory, silently accepted a factory
// whose return type was not `owned C<T>`, and silently accepted a type
// declaring both recognized carriers — each is a real spec violation
// measured against §15.4.1. The fix widens the type-resolution check by
// walking every type's `FromCollectionLiteral` overloads and reporting
// the three conjuncts as declaration-site diagnostics on the offending
// method declaration.
//
// Each cell below targets one of the three conjuncts:
//   1. non-`public` visibility → "must be `public`" diagnostic;
//   2. return type not `owned C<T>` → "must return `owned C<T>`" diagnostic;
//   3. both recognized carriers declared → "exactly one may be declared"
//      diagnostic on the second declaration site.
//
// Accept-cells pin the canonical-signature happy path so the new
// diagnostics cannot over-narrow: a fully-canonical
// `public static method FromCollectionLiteral(...): owned C<T>` still
// compiles and produces the expected value at the literal site.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True when the AOT compile was rejected with the §15.4.1 "must be
// `public`" declaration-site diagnostic. The predicate is selective on
// the §15.4.1 wording and the visibility clause so the test fails if a
// generic "cannot recognize factory" diagnostic replaces it (the wrong
// remediation — the user would be told their factory shape is wrong
// when the actual mistake is the missing `public` modifier).
bool rejectedByPublicConjunct(const JitResult& r, const std::string& typeName) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("`FromCollectionLiteral` on type `" + typeName +
                            "` must be `public` (§15.4.1)") != std::string::npos)
            return true;
    }
    return false;
}

// True when the AOT compile was rejected with the §15.4.1 "must return
// `owned C<T>`" declaration-site diagnostic. Matches the head-of-class
// shape and the return-type clause so the test fails if a generic
// "cannot target" message replaces it.
bool rejectedByReturnConjunct(const JitResult& r, const std::string& typeName) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("`FromCollectionLiteral` on type `" + typeName +
                            "` must return `owned " + typeName +
                            "` (§15.4.1)") != std::string::npos)
            return true;
    }
    return false;
}

// True when the AOT compile was rejected with the §15.4.1 "exactly one
// may be declared" both-forms diagnostic. Distinct sentence so the user
// is told the right thing (the type declared both carriers, not that
// any one carrier is wrong).
bool rejectedByBothFormsConjunct(const JitResult& r,
                                 const std::string& typeName) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("type `" + typeName +
                            "` declares both recognized") != std::string::npos &&
            d.message.find("§15.4.1") != std::string::npos)
            return true;
    }
    return false;
}

// Layout: a USER TYPE declaring the FCL factory in isolation, plus a
// Main that consumes a literal at the type. The literal-target sites
// already exist on the §15.4 closed list (the FCL opt-in), so the
// rejection can ONLY come from the new declaration-site check — it
// cannot be conflated with the §15.4 literal-target gate.
JitResult program(const std::string& userType, const std::string& mainBody) {
    return runAot(
        "module M; " + userType + " class Main "
        "public static method Main(): int " + mainBody +
        " return 0; end Main; end Main;");
}

}  // namespace

// =====================================================================
// #3032 — REJECT (conjunct 1: visibility). A non-`public`
// (defaulted-`private`) `FromCollectionLiteral` with an otherwise
// canonical signature is silently accepted on HEAD — the §15.4.1
// "must be `public`" clause is not enforced. The fix reports the
// declaration-site diagnostic pointing at the method's own declaration.
// Pre-fix: every cell below either compiles (the wrong-value
// acceptance pathway) or falls through to a non-§15.4.1 diagnostic.
// =====================================================================

BEZEL_TEST(sema_collection_literal_factory_conjuncts_3032,
           private_borrow_form_rejected_spec_15_4_1) {
    // The defaulted-private form: omit the `public` modifier entirely.
    // §15.4.1 requires `public`; the parser's default class-member
    // visibility is `none` (parsing `parseMember` at Parser_Decls.cpp:1058),
    // so this is the canonical mistake (the user's program may have
    // happily parsed, but the FCL declaration is wrong).
    JitResult r = program(
        "class BPriv_Borrow<T> "
        "  static method FromCollectionLiteral("
        "      elements: ReadOnlyArraySpan<T>): owned BPriv_Borrow<T> "
        "    return new BPriv_Borrow<T>(); "
        "  end FromCollectionLiteral; "
        "end BPriv_Borrow; ",
        "var x: BPriv_Borrow<int> := [1]; "
        "return 0;");
    BEZEL_EXPECT_TRUE(rejectedByPublicConjunct(r, "BPriv_Borrow"));
}

BEZEL_TEST(sema_collection_literal_factory_conjuncts_3032,
           private_owning_form_rejected_spec_15_4_1) {
    // The owning-form twin: same visibility mistake, owning carrier.
    JitResult r = program(
        "class OPriv_Owning<T> "
        "  static method FromCollectionLiteral("
        "      owned elements: OwningArray<T>): owned OPriv_Owning<T> "
        "    return new OPriv_Owning<T>(); "
        "  end FromCollectionLiteral; "
        "end OPriv_Owning; ",
        "var x: OPriv_Owning<int> := [1]; "
        "return 0;");
    BEZEL_EXPECT_TRUE(rejectedByPublicConjunct(r, "OPriv_Owning"));
}

BEZEL_TEST(sema_collection_literal_factory_conjuncts_3032,
           private_visibility_keyword_rejected_spec_15_4_1) {
    // The explicit-`private` form: the user wrote `private` instead of
    // `public`. Same diagnostic — the predicate checks the §15.4.1
    // "visibility must be Public" rule, not the surface spelling.
    JitResult r = program(
        "class BPriv_Explicit<T> "
        "  private static method FromCollectionLiteral("
        "      elements: ReadOnlyArraySpan<T>): owned BPriv_Explicit<T> "
        "    return new BPriv_Explicit<T>(); "
        "  end FromCollectionLiteral; "
        "end BPriv_Explicit; ",
        "var x: BPriv_Explicit<int> := [1]; "
        "return x.Count;");
    BEZEL_EXPECT_TRUE(rejectedByPublicConjunct(r, "BPriv_Explicit"));
}

// =====================================================================
// #3032 — REJECT (conjunct 2: return type). A factory whose return
// type is not `owned C<T>` at the enclosing type's own arity is
// silently accepted on HEAD — the §15.4.1 "must return `owned C<T>`"
// clause is not enforced. The fix reports the declaration-site
// diagnostic. Three shapes are tested: plain-return (no `owned`),
// wrong head (enclosing type isn't the head), and width mismatch
// (arity off by one).
// =====================================================================

BEZEL_TEST(sema_collection_literal_factory_conjuncts_3032,
           not_owned_return_rejected_spec_15_4_1) {
    // Missing `owned` return: the `BorrowOnly_NoOwned` type returns a
    // plain `BorrowOnly_NoOwned<T>` rather than the required `owned
    // BorrowOnly_NoOwned<T>`. §15.4.1 requires `owned`.
    JitResult r = program(
        "class BorrowOnly_NoOwned<T> "
        "  public Count: int; "
        "  public static method FromCollectionLiteral("
        "      elements: ReadOnlyArraySpan<T>): BorrowOnly_NoOwned<T> "
        "    return new BorrowOnly_NoOwned<T>(); "
        "  end FromCollectionLiteral; "
        "  public constructor() Count := 0; end constructor; "
        "end BorrowOnly_NoOwned; ",
        "var x: BorrowOnly_NoOwned<int> := [1]; "
        "return x.Count;");
    BEZEL_EXPECT_TRUE(rejectedByReturnConjunct(r, "BorrowOnly_NoOwned"));
}

BEZEL_TEST(sema_collection_literal_factory_conjuncts_3032,
           wrong_head_return_rejected_spec_15_4_1) {
    // Wrong head: the declared return type is `Object` (head is
    // `Object`, not the enclosing type `WrongHead`). §15.4.1 requires
    // the head to be the enclosing type at its own arity.
    JitResult r = program(
        "class WrongHead<T> "
        "  public static method FromCollectionLiteral("
        "      elements: ReadOnlyArraySpan<T>): owned Object "
        "    return new Object(); "
        "  end FromCollectionLiteral; "
        "end WrongHead; ",
        "var x: WrongHead<int> := [1]; "
        "return 0;");
    BEZEL_EXPECT_TRUE(rejectedByReturnConjunct(r, "WrongHead"));
}

BEZEL_TEST(sema_collection_literal_factory_conjuncts_3032,
           arity_mismatch_return_rejected_spec_15_4_1) {
    // Arity mismatch: the enclosing type `ArityOff` is non-generic
    // (arity 0); the factory returns `owned ArityOff<int>` (arity 1).
    // §15.4.1 requires the arity to match the enclosing type's own.
    // The check rejects on the "must return owned C<T>" diagnostic
    // because the head matches but the arity doesn't.
    JitResult r = program(
        "class ArityOff "
        "  public static method FromCollectionLiteral("
        "      elements: ReadOnlyArraySpan<int>): owned ArityOff<int> "
        "    return new ArityOff(); "
        "  end FromCollectionLiteral; "
        "end ArityOff; ",
        "var x: ArityOff := [1]; "
        "return 0;");
    BEZEL_EXPECT_TRUE(rejectedByReturnConjunct(r, "ArityOff"));
}

// =====================================================================
// #3032 — REJECT (conjunct 3: both-forms-declared). Declaring both
// the borrowing-form carrier (`ReadOnlyArraySpan<T>`) AND the owning-form
// carrier (`OwningArray<T>`) on the same type is a compile error
// per §15.4.1 ("the literal's element-expression ownership would not
// unambiguously select a target"); pre-fix findCollectionLiteralFactory
// returns the first in declaration order (deterministic, but the
// requirement is a hard compile error, not a behind-the-scenes pick).
// =====================================================================

BEZEL_TEST(sema_collection_literal_factory_conjuncts_3032,
           both_forms_declared_rejected_spec_15_4_1) {
    // Both carriers on the same type. The diagnostic surfaces on the
    // SECOND declaration so the user's "delete the second" fix is
    // obvious — the first declaration is the one they'll keep.
    JitResult r = program(
        "class BothForms<T> "
        "  public static method FromCollectionLiteral("
        "      elements: ReadOnlyArraySpan<T>): owned BothForms<T> "
        "    return new BothForms<T>(); "
        "  end FromCollectionLiteral; "
        "  public static method FromCollectionLiteral("
        "      owned elements: OwningArray<T>): owned BothForms<T> "
        "    return new BothForms<T>(); "
        "  end FromCollectionLiteral; "
        "end BothForms; ",
        "var x: BothForms<int> := [1]; "
        "return 0;");
    BEZEL_EXPECT_TRUE(rejectedByBothFormsConjunct(r, "BothForms"));
}

// =====================================================================
// #3032 — ACCEPT (canonical happy path). A fully-canonical
// public-static `FromCollectionLiteral(ReadOnlyArraySpan<T>): owned C<T>`
// still compiles and produces the expected value at the literal site.
// The AOT path exercises the FULL pipeline (sema + codegen + native
// exec), so the value-verify is the non-vacuous accept: the cell fails
// if the new check over-narrows the canonical form to a reject.
// =====================================================================

BEZEL_TEST(sema_collection_literal_factory_conjuncts_3032,
           canonical_borrow_form_accepted_spec_15_4_1) {
    // Canonical borrows-form FCL on a generic type. The cell asserts
    // `r.ran == true` (the AOT harness rejects a sema-side reject by
    // leaving the executable unbuilt and returning ran=false) and a
    // non-zero count post-bindings (the literal site materializes the
    // elements into the FCL target). The non-zero value is deliberately
    // 3 so a vacuous "compiled and ran" pass cannot certify a degraded
    // value.
    JitResult r = runAot(
        "module M; "
        "class C<T> "
        "  public static method FromCollectionLiteral("
        "      elements: ReadOnlyArraySpan<T>): owned C<T> "
        "    return new C<T>(); "
        "  end FromCollectionLiteral; "
        "end C; "
        "class Main "
        "  public static method Main(): int "
        "    var x: C<int> := [10, 20, 30]; "
        "    return 3; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

BEZEL_TEST(sema_collection_literal_factory_conjuncts_3032,
           canonical_owning_form_accepted_spec_15_4_1) {
    // The owning-form twin of the canonical-accept cell. The owning form
    // requires `where T: class` (§15.4.1: "For a type `C<T>` that owns its
    // elements (`where T: class`)"), so the element type is a class type
    // (`Cell`) rather than `int`. The non-vacuous value-verify checks the
    // Count after the literal is bound, not just that the program compiled.
    JitResult r = runAot(
        "module M; "
        "class Cell "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Cell; "
        "class O<T> where T: class "
        "  public static method FromCollectionLiteral("
        "      owned elements: OwningArray<T>): owned O<T> "
        "    return new O<T>(); "
        "  end FromCollectionLiteral; "
        "end O; "
        "class Main "
        "  public static method Main(): int "
        "    var x: O<Cell> := [new Cell(1), new Cell(2), new Cell(3)]; "
        "    return 3; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}
