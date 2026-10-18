// fix #6383 (§2.7 rule 1, §2.7 Through nesting, §11.6, §12.9, v0.338): a nested type named by
// its BARE name from inside an enclosing type's body composes its §12.9
// position like any other head.
//
// THE SHAPE, AND WHY IT LOOKS THE WAY IT DOES. A type that can CARRY a nested
// type is never a type that can CARRY the `in`/`out` parameter. §11.8 restricts
// an interface body to methods, properties, events and statics, so an interface
// admits no nested type — MEASURED: `interface I<out T> interface Box<in X> …
// end Box; end I;` gives `expected 'method', 'property', 'event', or 'static
// method'/'static operator' (§11.8) in interface body`. And §12.9's `in`/`out`
// are interface-only syntax, so only an interface has a variant parameter at
// all. §11.6 puts nested types in a CLASS body ("A class body may declare nested
// classes, records, enums, and interfaces"), in a RECORD body ("Nested types in
// records are permitted but uncommon"), and in a §21.10 local record ("A local
// record's own body may itself contain §11.6 nested member types"). All three
// families are pinned here — class by most rows, record by row 12, local record
// by row 13 — because they are three different ways the same rule is reached.
//
// So the shape this file pins is an enclosing type carrying the nested head, and
// a nested interface `Holder<out T>` whose member signature names that head by
// BARE NAME — reached through §2.7's "Through nesting" ("the enclosing-type
// lookup visits the nested type and then each type enclosing it, innermost
// first").
//
// THE DEFECT, IN TWO PARTS. Measured on base `5df02d87`, `Holder<out T>`'s
// `method Get(): Box<T>` against `Box<in X>` COMPILED, RAN and exited 0.
//
//   1. `checkVariancePositions` walked `ctx_.module()->children` ONLY, so a
//      NESTED interface's body was never checked at all. §12.9's rule states no
//      nesting exemption — a body-position `Box<T>` resolving to `Box<in X>`
//      under an `out T` return slot is the same violation whether or not the
//      interface declaring the slot is itself nested.
//   2. Even with the body walked, the HEAD was unanswerable. A bare name
//      reaches exactly two arms of `namedHeadArgVariance`: the local TOP-LEVEL
//      table, fed `localTypes` = `ctx_.module()->children`, and
//      `resolveImportedHead`'s bare-name arm, fed a module's EXPORTS. Neither
//      can ever hold a nested type. §2.7 rule 1's step between them — "local
//      scope, THEN THE ENCLOSING TYPE'S ACCESSIBLE MEMBERS, then the current
//      module's declarations" — was not modelled at all.
//
// #6369's `resolveLocalNestedHead` does not cover this: it is reached only on a
// `LocalTopLevel` verdict, which #6373's `classifyHeadQualifier` returns only
// for a path of TWO OR MORE segments. A bare name is `Bare`, and answered by
// nothing.
//
// THE FIX is one arm, `enclosingMemberBareName`, fed `heads.enclosingChain` —
// the chain of types enclosing the use site, innermost first — placed FIRST in
// the bare-name branch, above the top-level table §2.7 rule 1 ranks it below.
//
// THE ROWS. Every one is single-module and asserts the exact diagnostic or
// `r.ran`. The pairs are the point:
//
//   * rows 1/3, 2/4, 8/9, 10/11, 13/14 and 15/16 are marker-flipped pairs at
//     the same spelling. Row 1 (rejected) and row 3 (accepted) differ ONLY in
//     the nested head's own polarity; rows 8/9 differ ONLY in which of a nested
//     and a TOP-LEVEL type of the same name carries which marker; rows 10/11
//     differ ONLY in which of TWO enclosing types carries the nested head; rows
//     13/14 are the same rule reached through a RECORD body; rows 15/16 reach
//     it through a §21.10 method-LOCAL record. An arm that read a position off
//     the wrong thing — the enclosing class, the OUTERMOST enclosing type
//     rather than the innermost, or the top-level table rather than the
//     enclosing-type step — answers wrongly on one side of some pair.
//     Rows 1, 9, 13 and 15 alone cannot tell any of them apart.
//   * row 5 is the KIND control: a nested CLASS is always invariant (§12.9), so
//     the same spelling is rejected even though `Holder` is covariant. Only an
//     arm that reads the nested type's own `kind` gets that.
//   * row 6 is the no-spurious-diagnostic control: the spelling names no type
//     this pass may read a marker off, so §12.9 must contribute NOTHING — an arm
//     that answered anyway would add a second, spurious §12.9 to a program the
//     name resolver rejects on its own account, which is the shape #6351's
//     first attempt was REJECTED for.
//   * rows 7 and 12 bound the arm: row 7 says unrelated members on the
//     enclosing types do not stop the walk, row 12 that with nothing nested the
//     pre-existing top-level table still answers. Together they say the new arm
//     neither over-reaches nor blanket-suppresses.

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
const char* kOutInInput = "is covariant (`out`) but appears in an input (parameter) position";
const char* kInOutput = "is contravariant (`in`) but appears in an output (return) position";

constexpr const char* kMain =
    "class Program "
    "  public static method Main() Console.WriteLine(\"x\"); end Main; "
    "end Program; ";

std::string program(const std::string& decls) {
    return "module M; import System; " + decls + kMain;
}

// `Outer` carrying a NESTED head `Box<nestedMarker X>` (of `nestedKind`), and a
// NESTED interface `Holder<marker T>` whose own member signature names that
// head by BARE NAME in a return slot — the spelling this issue is about.
std::string nested(const char* marker, const char* nestedKind,
                   const char* nestedMarker, const char* nestedBody) {
    return std::string("public class Outer ") +
           "public " + nestedKind + " Box<" + nestedMarker + " X> " + nestedBody +
           " end Box; " +
           "public interface Holder<" + marker + " T> method Get(): Box<T>; "
           "end Holder; end Outer; ";
}

// REJECT with §12.9, and with the polarity sentence the row is about.
//
// `src` is the WHOLE program. Nothing here asserts the absence of any other
// diagnostic and nothing counts diagnostics: the §21.10 rows (15/16) carry a
// name-resolution diagnostic alongside §12.9 today, which a later resolver fix
// will remove, so an absence- or count-assertion over the full diagnostic set
// would be red on day one and would have to be deleted when that lands. These
// four assertions are exactly "this rejected, for this reason, with this
// polarity" and nothing more.
void expectVariance129On(const std::string& src, const char* polarity) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, polarity));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kComposedClause));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kVariance129));
}

void expectVariance129(const std::string& decls, const char* polarity) {
    expectVariance129On(program(decls), polarity);
}

// REJECT on some OTHER rule, with §12.9 contributing nothing.
void expectRejectedWithout129(const std::string& decls, const char* other) {
    auto r = runAot(program(decls));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, other));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kVariance129));
}

// REJECT on the NAME RESOLVER, with §12.9 contributing nothing.
//
// This is NOT a weakened `expectAccepted`: it is the polarity discriminator for
// a shape the resolver rejects outright. `r.ran` is deliberately not asserted
// and no diagnostic is counted — see the §21.10 rows below for why this shape
// can never run, and why that must not be pinned in either direction. What IS
// asserted is that §12.9 fires on the contravariant spelling of this same
// program (its twin) and does NOT fire here, so an arm that always answered
// with a position, or that read the polarity off the wrong thing, is caught.
void expectNoVariance129(const std::string& src) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kVariance129));
}

// ACCEPT: the composition is sound, so the declaration must compile clean.
void expectAccepted(const std::string& decls) {
    auto r = runAot(program(decls));
    BEZEL_EXPECT_TRUE(r.ran);
}

}  // namespace

// --- 1. the violation that used to compile ---------------------------------
//
// `Box<in X>` puts its argument in a CONTRAVARIANT slot; `Get()` is a RETURN
// slot, so `Holder`'s `T` composes to contravariant, which `out T` cannot
// honour. On the base this compiled clean, ran, and exited 0.

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           contravariant_nested_bare_head_under_out_return_rejected) {
    expectVariance129(nested("out", "interface", "in", "method Put(x: X);"),
                      kOutInInput);
}

// --- 2. the mirror violation: `in T` through a COVARIANT bare head ----------

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           covariant_nested_bare_head_under_in_return_rejected) {
    expectVariance129(nested("in", "interface", "out", "method Get(): X;"),
                      kInOutput);
}

// --- 3. THE CONTROL that makes rows 1 and 2 discriminating ------------------
//
// Byte-identical to row 1 except the nested head's own polarity. Only an arm
// that reads `Box<out X>`'s marker COVARIANT makes this compile. An arm that
// read the position off the ENCLOSING CLASS `Outer` — a class is always
// invariant, §12.9 — would reject it.

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           covariant_nested_bare_head_under_out_return_accepted) {
    expectAccepted(nested("out", "interface", "out", "method Get(): X;"));
}

// --- 4. and the sound composition: contravariant-of-contravariant -----------

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           contravariant_nested_bare_head_under_in_return_accepted) {
    expectAccepted(nested("in", "interface", "in", "method Put(x: X);"));
}

// --- 5. the KIND is read off the NESTED head, not off the enclosing class ---
//
// The nested head is a CLASS. §12.9: a class is always invariant, so `T`
// composes to Invariant and `out T` cannot honour it — rejected, even though
// `Holder` is itself covariant. An arm that stopped at the enclosing class
// would answer CoVariant and accept this row.

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           class_nested_bare_head_under_out_return_rejected) {
    expectVariance129(nested("out", "class", "", "method Take(): X;"),
                      kOutInInput);
}

// --- 6. a bare name no enclosing type carries: §12.9 says nothing ------------
//
// Nothing named `Nope` on `Holder`, `Outer`, or in the module, so §2.7 rule 1's
// search finds nothing and the name resolver reports the spelling. A §12.9
// line here would be a false resolve — #6351's first attempt's shape.

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           absent_nested_bare_head_adds_no_variance) {
    expectRejectedWithout129(
        "public class Outer public interface Holder<out T> "
        "  method Get(): Nope<T>; "
        "end Holder; end Outer; ",
        "unresolved type name `Nope`");
}

// --- 7. unrelated members on the enclosing types do not block the walk ------
//
// `Holder` and `Outer` each carry an ordinary method alongside the nested head.
// The chain walk must still reach `Outer's` nested `Box` and reject — an arm
// that treated any member of the enclosing types as ending the search (a
// reading of §2.7's "ending the search" sentence this lane MEASURED the name
// resolver not taking: `Holder` declaring a method `Box` does NOT stop
// `Box<T>` binding to `Outer`'s nested type) would answer nothing here.

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           unrelated_enclosing_members_do_not_block_the_nested_walk) {
    expectVariance129(
        "public class Outer "
        "  public marker: int; "
        "  public interface Box<in X> method Put(x: X); end Box; "
        "  public interface Holder<out T> "
        "    method Other(): int; "
        "    method Get(): Box<T>; "
        "  end Holder; "
        "end Outer; ",
        kOutInInput);
}

// --- 8/9. THE PRECEDENCE ROWS: a nested `Box` AND a top-level `Box` ---------
//
// §2.7 rule 1 ranks "the enclosing type's accessible members" above "the current
// module's declarations", so where both exist the NESTED one is the binding.
// These two rows are that claim, and nothing else in the file tests it: every
// other row has a unique name per scope. A reviewer-planted mutant that puts the
// pre-existing top-level table FIRST — the exact opposite of the commit's
// placement — SURVIVED the ten rows these were added to.
//
// The two rows are a marker-flipped pair at the same spelling, so each kills the
// mutant in the direction the other would miss:
//   row 8, nested COVARIANT + top-level CONTRAVARIANT -> ACCEPTED. Reading the
//     top-level head instead composes ContraVariant and rejects.
//   row 9, nested CONTRAVARIANT + top-level COVARIANT -> REJECTED §12.9. Reading
//     the top-level head instead composes CoVariant and accepts.
//
// MEASURED that the name resolver agrees — `interface Probe method M(b:
// Box<int>);` nested in each program compiles clean, so it also binds the bare
// `Box` to the enclosing type's nested type. Check and resolver are consistent
// today; these rows pin that consistency as well as the placement.

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           nested_covariant_head_shadowed_by_top_level_contravariant_accepted) {
    expectAccepted(
        "public interface Box<in X> method Put(x: X); end Box; "   // top-level
        "public class Outer "
        "  public interface Box<out X> method Get(): X; end Box; "  // nested
        "  public interface Holder<out T> method Get(): Box<T>; end Holder; "
        "end Outer; ");
}

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           nested_contravariant_head_shadowed_by_top_level_covariant_rejected) {
    expectVariance129(
        "public interface Box<out X> method Get(): X; end Box; "   // top-level
        "public class Outer "
        "  public interface Box<in X> method Put(x: X); end Box; "  // nested
        "  public interface Holder<out T> method Get(): Box<T>; end Holder; "
        "end Outer; ",
        kOutInInput);
}

// --- 10. TWO enclosing types carry the head: the INNERMOST answers -----------
//
// `Outer` and `Mid` both declare a nested `Box`, and `Holder` sits in `Mid`.
// §2.7: "the enclosing-type lookup visits the nested type and then each type
// enclosing it, INNERMOST FIRST … the first type that has an accessible member
// of that name ends the search." `Mid.Box` is COVARIANT, so this compiles. An
// arm that walked the chain outermost-first would land on `Outer.Box`
// (contravariant) and reject — the mirror of row 3's mutant.

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           innermost_enclosing_nested_head_answers_covariantly) {
    expectAccepted(
        "public class Outer "
        "  public interface Box<in X> method Put(x: X); end Box; "
        "  public class Mid "
        "    public interface Box<out X> method Get(): X; end Box; "
        "    public interface Holder<out T> method Get(): Box<T>; end Holder; "
        "  end Mid; "
        "end Outer; ");
}

// --- 11. the mirror of row 10: flip only the INNERMOST marker ---------------

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           innermost_enclosing_nested_head_answers_contravariantly) {
    expectVariance129(
        "public class Outer "
        "  public interface Box<out X> method Get(): X; end Box; "
        "  public class Mid "
        "    public interface Box<in X> method Put(x: X); end Box; "
        "    public interface Holder<out T> method Get(): Box<T>; end Holder; "
        "  end Mid; "
        "end Outer; ",
        kOutInInput);
}

// --- 12. the TOP-LEVEL control: nothing nested, and it still composes -------
//
// With no nested type of that name on any enclosing type, §2.7 rule 1's search
// reaches "the current module's declarations" and the pre-existing table answers
// exactly as it did before this fix. Row 12 is what says the new arm is not a
// blanket suppressor of the arm it sits above.

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           top_level_bare_head_still_composes_inside_a_nested_interface) {
    expectVariance129(
        "public interface Lib<in X> method Put(x: X); end Lib; "
        "public class Outer public interface Holder<out T> "
        "  method Get(): Lib<T>; "
        "end Holder; end Outer; ",
        kOutInInput);
}
// --- 13/14. the RECORD family: the same rule reached through another body ----
//
// Every other row carries the enclosing type in a CLASS body. §11.6 names two
// more: "Nested types in records are permitted but uncommon", and a §21.10 local
// record's "own body may itself contain §11.6 nested member types". The record
// family is a DIFFERENT traversal path — a top-level `TypeDeclKind::Record` that
// the old loop skipped by its `kind != Interface` filter just as it skipped a
// class — so it deserves its own marker-flipped pair rather than an assumption
// that the class rows cover it. MEASURED: silent on the base, and these two rows
// reject and accept respectively on the fix.
//
// Row 13 is the violation; row 14 is the control that makes it discriminating
// (a nested CLASS head read off the wrong type, §12.9's "a class is always
// invariant", would reject row 14).

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           record_enclosed_contravariant_bare_head_under_out_return_rejected) {
    expectVariance129(
        "public record R "
        "  public interface Bad<in X> method Put(x: X); end Bad; "
        "  public interface H<out T> method Get(): Bad<T>; end H; "
        "end R; ",
        kOutInInput);
}

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           record_enclosed_covariant_bare_head_under_out_return_accepted) {
    expectAccepted(
        "public record R "
        "  public interface Bad<out X> method Get(): X; end Bad; "
        "  public interface H<out T> method Get(): Bad<T>; end H; "
        "end R; ");
}

// --- 15/16. the §21.10 LOCAL-RECORD family: the bare spelling, pinned ---------
//
// Rows 13/14 reach a record body from a module top-level. §11.6 also says a
// §21.10 local record's "own body may itself contain §11.6 nested member
// types", which is a THIRD traversal path: this record is not a module
// top-level at all, so the variance pass can only reach it through the
// enclosing type's `NestedTypeSymbol` members. It gets its own pair rather than
// an assumption that 13/14 cover it. MEASURED: silent on the base; these reject
// and accept respectively on the fix.
//
// THIS is the pair that pins the bare-name spelling itself — see the block
// below for the discriminator. Row 15 is the violation; row 16 flips ONLY the
// nested head's own marker, and without it row 15 would pass under any arm that
// answers with a position at all (the vacuous-oracle shape): a constant, a
// wrong-type read, or an arm that always says "input" all satisfy row 15 and
// are caught only by row 16.
//
// ROW 16'S ORACLE IS "NO §12.9", NOT `r.ran`. MEASURED: the name resolver
// rejects `Bad<T>` inside a method-local record at BOTH polarities — the
// covariant spelling emits "unresolved type name 'Bad'" at 6:47 and no §12.9,
// the contravariant one emits both — so this shape can never `run` whatever the
// variance pass says, and an `expectAccepted` here could never pass. The
// polarity is still pinned tightly: the two rows differ only in the nested
// head's own marker, and §12.9's presence flips across exactly that one token.
// A PRE-EXISTING §21.10 misfire (on base too) fires on both; neither row looks
// at it, or at `unresolved type name`, either way.
//
// The program text is built whole here rather than through `program()`, because
// this row's `Program` IS the program — appending `kMain` would declare a
// second one. `static` because it lives at file scope, outside the anonymous
// namespace, and the name is generic enough to collide.
static std::string localRecord(const char* nestedMarker, const char* nestedBody) {
    return std::string("module M; import System; public class Program "
                       "public static method Main() "
                       "record Local "
                       "public interface Bad<") +
           nestedMarker + " X> " + nestedBody +
           " end Bad; "
           "public interface H<out T> method Get(): Bad<T>; end H; "
           "end Local; "
           "end Main; end Program; ";
}

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           local_record_nested_bare_head_under_out_return_rejected) {
    expectVariance129On(localRecord("in", "method Put(x: X);"), kOutInInput);
}

BEZEL_TEST(nested_type_bare_name_body_variance_6383,
           local_record_covariant_bare_head_emits_no_variance) {
    expectNoVariance129(localRecord("out", "method Get(): X;"));
}

// ⚠ MEASURED — the §21.10 local-record family, and what the bare-name rows pin.
//
// A §21.10 method-local `record Local` holding `interface Bad<in X>` +
// `interface H<out T>.Get(): Bad<T>` (BARE spelling) is SILENT on the base and
// REJECTED §12.9 with this fix. Rows 15/16 above pin exactly that.
//
// The same program spelled `Local.Bad<T>` (QUALIFIED) also emits §12.9 today,
// but that §12.9 is NOT this commit's — it is served by #6369's
// `resolveLocalNestedHead`, which is already on base and which this commit does
// not touch. MEASURED by discriminator, not by reading: with this commit's arm
// neutered (`BEZEL_NEUTER_ARM` in `enclosingMemberBareName`, built and probed
// on this branch), the QUALIFIED spelling still emits §12.9 and the BARE
// spelling loses its §12.9 entirely. So a row on the qualified spelling is not
// a regression test for this commit — it stays green with the arm removed — and
// nothing here pins it. That is the whole reason; it is not a claim that
// pinning it would invert.
//
// WHAT THE BARE-SPELLING DIVERGENCE IS. The name resolver rejects `Bad<T>`
// inside a method-local record today (`unresolved type name 'Bad'`), emitted
// ALONGSIDE the §12.9. So for the bare spelling this checker gets FURTHER than
// the resolver — which is the direction the fix intends (it answers a spelling
// the resolver cannot yet bind), not a defect this commit introduces. Rows
// 15/16 assert the §12.9 and deliberately say nothing about the resolver's own
// diagnostic, so they survive the resolver fix unchanged. A PRE-EXISTING §21.10
// misfire also fires on this shape, on base too — `local type references the
// enclosing generic parameter `X`` for `Bad`'s OWN parameter — and is likewise
// unasserted either way.
//
// The GENUINE pre-existing divergence, which this commit does NOT introduce and
// does not fix, is on the QUALIFIED side: `Local.Bad<T>` gets a §12.9 from
// #6369 while the resolver rejects the same spelling, so that one composes a
// position for a type the resolver does not bind. Reported to the coordinator
// so the eventual `TypeResolution_Names.cpp` fix carries
// `enclosingMemberBareName` with it.
