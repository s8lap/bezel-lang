// fix #6383 follow-up (§2.7 "Through nesting", §2.7 warning row, §12.9, §13.1,
// v0.338): §2.7's enclosing-member STOP ends §12.9's nested-head composition too.
//
// THE DEFECT. #6383 (`7b1b972f`) added `enclosingMemberBareName` to
// `src/sema/TypeResolution_Checks.cpp` so §12.9 can compose a variance position
// through a nested type named by its BARE name from inside the enclosing type's
// own body. That arm reads only `NestedTypeSymbol` on the enclosing chain, so
// it walks straight past a METHOD or PROPERTY of the colliding name. Its own
// comment said so, and said what had to happen next: "This function therefore
// matches the resolver rather than §2.7, and MUST be changed with it: when the
// resolver is fixed to end the search, this must end it too, or the two will
// disagree."
//
// MEASURED on a COMPOSED tree (#6383 + the §2.7 name-resolver fix, built out of
// tree), the coupling program carries BOTH diagnostics:
//
//   public class Outer
//     public interface Box<in X> method Put(x: X); end Box;
//     public interface Holder<out T>
//       method Box(): int;
//       method Get(): Box<T>;
//     end Holder;
//   end Outer;
//
//   unresolved type name `Box`; `Box` is a member of the enclosing type
//   `Holder` — a bare name's search ends at the first enclosing type that has
//   it, whatever kind of member that is (§2.7 Through nesting); …
//   type parameter `T` is covariant (`out`) but appears in an input (parameter)
//   position (composed through a nested type constructor) in method `Get`
//   (§12.9)
//
// §2.7 says the search STOPS, so there is no longer a resolved head and §12.9
// has nothing to compose. It is not an over-rejection — the program is rejected
// either way — but it is a second, spurious diagnostic on an already-rejected
// program, and it puts two rules in contradiction about which owns the verdict.
//
// THE SHAPE EVERY ROW HERE USES, and why it is not a bare absence-assertion.
// `runAot` fills `JitResult::diagnostics` only on a REJECT (#3138), so asserting
// "§12.9 did not fire" on a program that COMPILES would be vacuous — and on this
// branch the stopped program does exactly that compile, because the §2.7
// name-resolver half is a separate, file-disjoint commit. So each program below
// carries TWO interfaces over ONE nested head:
//
//   * `H` — nested in a class that declares a member named `Bad` (or `Bad` in
//     its own body), so §2.7's stop ends the chain walk for it;
//   * `W` — nested in a sibling class that declares nothing of that name, so
//     its walk reaches `Outer`'s nested `Bad<in X>` and §12.9 fires for it.
//
// Every row asserts BOTH halves: `GetW` present and `GetH` absent (or both
// present, for a row about a member that must NOT stop the search). `W` is the
// anti-vacuity witness — it is what proves the variance pass ran and produced a
// §12.9 diagnostic in this very program, so `GetH`'s absence is a fact about the
// stop rather than an empty diagnostic set.
//
// ⚠ THE ROWS ARE ANCHORED TO §2.7's RULE, NOT TO ANY OTHER COMMIT'S SHA. They
// assert what §2.7 says about a bare TYPE-name search in a nested type's body,
// and nothing about whether the name resolver has been changed to agree yet.
// Measured, on this branch (the resolver half absent) and on the composed tree
// (the resolver half present), they pass identically. A row asserting the
// §2.7 name-resolver diagnostic's own text would be RED here and would have to
// be rewritten when that half lands; there is deliberately no such row.
//
// WHICH ROWS CARRY THE FIX, and which guard against over-reach. MEASURED by
// reverting `src/sema/TypeResolution_Checks.cpp` to `r30-c1`, rebuilding, and
// re-running: rows 1, 2 and 3 are RED without it (each on
// `BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kStoppedMethod))`, i.e.
// `GetH` composing when §2.7 says the search ended); rows 4, 5 and 6 are GREEN
// both ways, by design — each asserts a member that must NOT end the search, so
// they are the over-reach guards and would be the first to go red if the stop
// were applied too widely.
//
// NOT COVERED, declared rather than assumed: BASE types (§2.7's ranking
// paragraph says "inherited included", but the paragraph that STATES the stop
// enumerates the chain as "the nested type and then each type enclosing it",
// with no base types in it, and `baseTypes` is a separate accessor), and §21.10
// local types (whose bodies contribute no chain element here). Both are wider
// than the paragraph that states the stop.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains, failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// §12.9's diagnostic, quoted whole: the head is named only by the composed
// clause, so a row that wants "§12.9 contributed nothing" has to look for the
// section mark and the composed clause together.
const char* kVariance129 = "(§12.9)";
const char* kComposedClause = "composed through a nested type constructor";

// The two method names, quoted as §12.9's diagnostic spells them. Neither is a
// substring of the other, so a row can assert one present and the other absent.
const char* kStoppedMethod = "in method `GetH`";
const char* kWitnessMethod = "in method `GetW`";

constexpr const char* kMain =
    "public class Program "
    "  public static method Main() Console.WriteLine(\"x\"); end Main; "
    "end Program; ";

std::string program(const std::string& decls) {
    return "module M; import System; " + decls + kMain;
}

// `Outer` carrying the nested contravariant head `Bad<in X>`; a class `Stop`
// that declares `stopDecl` and nests the interface whose member names `Bad` by
// bare name; and the witness class `NoStop`, which nests `W` and declares
// nothing of that name.
std::string stopAndWitness(const std::string& stopDecl) {
    return std::string("public class Outer ") +
           "public interface Bad<in X> method Put(x: X); end Bad; "
           "public class Stop " + stopDecl +
           "public interface H<out T> method GetH(): Bad<T>; end H; "
           "end Stop; "
           "public class NoStop "
           "public interface W<out T> method GetW(): Bad<T>; end W; "
           "end NoStop; "
           "end Outer; ";
}

// §2.7's STOP fires for `H` and not for `W`: the witness composes, `H` does not.
void expectStopEndsOnlyTheStoppedChain(const std::string& decls) {
    auto r = runAot(program(decls));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The anti-vacuity half: the variance pass ran and produced a §12.9
    // diagnostic in THIS program, for the interface whose chain reaches the
    // nested head.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kVariance129));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kComposedClause));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kWitnessMethod));
    // The row's actual claim.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kStoppedMethod));
}

// The member must NOT end the search, so BOTH interfaces compose.
void expectBothCompose(const std::string& decls) {
    auto r = runAot(program(decls));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kVariance129));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kComposedClause));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kWitnessMethod));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kStoppedMethod));
}

}  // namespace

// --- 1. THE COUPLING ITSELF ------------------------------------------------
//
// #6383's own regression shape, with `Holder` also declaring `method Box`. §2.7
// "Through nesting": "the first type that has an ACCESSIBLE MEMBER of that name
// ends the search … Ending the search at the first type that HAS THE NAME,
// EVEN WHEN THE MEMBER CANNOT BE USED THERE, keeps a bare name from passing
// over an enclosing member". A type position can use nothing at all at the end
// of that search, so there is no head and §12.9 composes nothing.
//
// Without the fix the program carries a §12.9 line for `GetH` as well; the
// witness `GetW` composes either way, so the row discriminates on `GetH` alone.

BEZEL_TEST(enclosing_member_bare_name_variance_stop,
           spec_2_7_through_nesting_method_of_the_name_ends_the_composition) {
    expectStopEndsOnlyTheStoppedChain(
        stopAndWitness("public method Bad(): int return 1; end Bad; "));
}

// --- 2. THE MARKER-FLIPPED TWIN OF ROW 1 ----------------------------------
//
// Row 1 with the head's marker flipped: `Bad<out X>` instead of `Bad<in X>`, so
// a return slot composes CoVariant and both interfaces declare `in T` — a real
// violation on BOTH sides, at the other polarity. MEASURED that this is what
// makes the row discriminate at all: with `H<out T>` (row 1's marker) `H` is
// covariant-of-covariant, §12.9 has nothing to say about it, and the row passes
// whether or not the stop fires — a row that cannot fail.
//
// A fix that ended the chain for some reason OTHER than the member of the name —
// a polarity read, a nesting depth, a marker — would let `H` through here.

BEZEL_TEST(enclosing_member_bare_name_variance_stop,
           spec_2_7_the_stop_is_not_a_polarity_or_marker_effect) {
    expectStopEndsOnlyTheStoppedChain(
        "public class Outer "
        "  public interface Bad<out X> method GetX(): X; end Bad; "
        "  public class Stop "
        "    public method Bad(): int return 1; end Bad; "
        "    public interface H<in T> method GetH(): Bad<T>; end H; "
        "  end Stop; "
        "  public class NoStop "
        "    public interface W<in T> method GetW(): Bad<T>; end W; "
        "  end NoStop; "
        "end Outer; ");
}

// --- 3. THE CARVE-OUT'S NEGATIVE TWIN -------------------------------------
//
// §2.7's warning row exempts a member "unless the member's declared type is the
// resolved type … the `property Color: Color` idiom". `property Bad: int` is
// NOT that idiom — its declared type's head is not the colliding name — so it
// IS a blocker and the search ends at `Stop`. Row 4 is the positive twin, and
// the two rows together are what make "the carve-out" a rule about the DECLARED
// TYPE rather than a rule about properties.
//
// Without this fix `GetH` composes and the row is red on it.

BEZEL_TEST(enclosing_member_bare_name_variance_stop,
           spec_2_7_a_member_typed_by_something_else_still_ends_the_search) {
    expectStopEndsOnlyTheStoppedChain(
        stopAndWitness("public property Bad: int := 0; "));
}

// --- 4. §2.7's `property Color: Color` CARVE-OUT ---------------------------
//
// The same program with `property Bad: Bad<int>?` — the declared type's head IS
// the colliding name, which is §2.7's named carve-out. The member is therefore
// not a blocker, the walk continues past `Stop` to `Outer's` nested
// `Bad<in X>`, and BOTH interfaces compose.
//
// It has to be a carve-out rather than a coincidence: an enclosing-member step
// that read such a member as a blocker would make the member UN-DECLARABLE —
// `property Bad: Bad<int>` would end the search for its own declared type.
//
// MEASURED: without the carve-out this program is rejected where the base
// accepts it, i.e. the row flips to `expectStopEndsOnlyTheStoppedChain` and
// then red on `GetH`. The `?` is §7 nullability, not part of the carve-out: it
// is here only so the property's `:= null` initializer type-checks, and the
// declared type's HEAD is `Bad` either way.

BEZEL_TEST(enclosing_member_bare_name_variance_stop,
           spec_2_7_property_color_color_carve_out_does_not_end_the_search) {
    expectBothCompose(stopAndWitness("public property Bad: Bad<int>? := null; "));
}

// --- 5. ACCESSIBILITY (§13.1) ---------------------------------------------
//
// `private` is reachable only within the DECLARING type (§13.1), and `H` is
// nested inside `Stop`, not inside it — so the member is not accessible from
// `H`'s body, does not end the search there, and the walk reaches `Outer`'s
// nested head for `H` exactly as it does for `W`.
//
// Row 1 is the `public` twin of this row on the same spelling, so the two are a
// pair about ACCESSIBILITY and not about whether the compiler can see the
// member at all. Filtering `private` unconditionally would reject row 1.

BEZEL_TEST(enclosing_member_bare_name_variance_stop,
           spec_2_7_private_enclosing_member_is_not_accessible_so_does_not_stop) {
    expectBothCompose(
        stopAndWitness("private method Bad(): int return 1; end Bad; "));
}

// --- 6. THE SCOPE BOUNDARY: A TOP-LEVEL TYPE'S OWN BODY -------------------
//
// §2.7's paragraph stating the stop opens "**Through nesting.** IN A NESTED
// TYPE'S BODIES (§11.6), the enclosing-type lookup visits the nested type and
// then each type enclosing it". `H` here is a MODULE TOP-LEVEL interface, so it
// is rule 1 alone — and rule 1's normative table has no row for a type position
// (its rows are a chain head, a bare value read, an assignment target, a bare
// callee, a head continued by `?.`/`!`, and the warning row), every one a
// position at which a NAME is read. So `H`'s own `method Bad` does not end the
// search, and it composes like `W` does.
//
// Without this scope guard the chain of ONE element would be treated as a
// nested body and this row would go red on `GetH`. It is also what keeps §2.7's
// own `property Color: Color` idiom spellable at the top level, which is pinned
// upstream by
// `codegen_choice_body_record_ctor_store_2097.spec_2_7_dotted_head_member_typed_as_colliding_type_accepted`
// and by
// `enclosing_member_bare_type_name.spec_2_7_property_color_color_carve_out_in_a_top_level_body`.
//
// ⚠ The module-level `Bad<in X>` is a SEPARATE declaration from the one `H`'s
// own member of that name blocks; they do not collide (§11.3's uniqueness is
// per type body, and `H`'s is its own).

BEZEL_TEST(enclosing_member_bare_name_variance_stop,
           spec_2_7_a_toplevel_types_own_member_does_not_end_the_search) {
    expectBothCompose(
        "public interface Bad<in X> method Put(x: X); end Bad; "
        "public interface H<out T> "
        "  method Bad(): int; "
        "  method GetH(): Bad<T>; "
        "end H; "
        "public interface W<out T> method GetW(): Bad<T>; end W; ");
}