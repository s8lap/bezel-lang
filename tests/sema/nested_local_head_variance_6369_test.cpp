// fix #6369 (§11.6, §2.7 rule 1, §12.9, v0.338): a §11.6 NESTED LOCAL head
// (`Outer.Inner`) composes its variance like any other head.
//
// §11.6: a nested type "is referenced from outside as `Outer.Nested` … the
// enclosing path is part of its qualified name" — so the QUALIFIED spelling is
// the ordinary one for a nested head. §12.9's composed position check names no
// such distinction ("Position is composed across nested type constructors. A
// type parameter inside another generic type inherits position from that
// wrapper's variance"), and the marker is read from the same
// `GenericParamSymbol::variance` field on both sides (#3143), so a nested LOCAL
// head composes exactly as a nested IMPORTED one does.
//
// BEFORE THIS FIX, no arm of `namedHeadArgVariance`
// (src/sema/TypeResolution_Checks.cpp) answered that spelling:
//   * the local-top-level table compares `parts.back()` and is gated to BARE
//     names since #6373;
//   * `resolveLocalAliasHead` matches single-part names only (§27.8 aliases are
//     module-level);
//   * `resolveImportedHead` returns null outright for a `LocalTopLevel`
//     qualifier — by design, since §2.2 lets a local declaration shadow an
//     import;
//   * the built-in table answers bare names only (#6373).
// So `Outer.Inner<T>` resolved to `Unknown` and §12.9 SKIPPED it. Measured on
// the base: `out T` in a RETURN slot against a contravariant
// `Outer.Box<in T>` compiled, ran and exited 0. A false skip on a real
// violation — the same class as #6373's Finding 2, and NOT the "safe direction"
// it was first assumed to be.
//
// THE FIX adds `resolveLocalNestedHead`: after `classifyHeadQualifier` has
// decided the leftmost segment binds to a local top-level TYPE (§2.7 rule 1),
// the rest of the path is walked with `walkNestedTypePath` — `resolveNamed`'s
// own §11.6 descent, the same function the imported arm uses. So the two arms
// cannot disagree about what a dotted spelling names; only the LOOKUP of the
// qualifying type differs (this module's own declarations vs a module's
// exports), and that difference is decided once, by the qualifier.
//
// EVERY ROW is single-module and asserts the exact diagnostic or `r.ran`. The
// pairs are the point:
//
//   * rows 1/3 and 2/4 are marker-flipped pairs at the same spelling. Row 1
//     (rejected) and row 3 (accepted) differ ONLY in the nested head's own
//     polarity, so an implementation that stopped the walk at the ENCLOSING
//     CLASS would answer `Invariant` for both and reject row 3 — that mutant
//     was measured green on a single reject row in #6373's review and is the
//     exact reason the accept row exists here.
//   * rows 5 and 6 are the no-spurious-diagnostic controls: the spelling names
//     no type the name resolver binds (§11.3 "no nested type named `Nope`", and
//     §27.8 for a local ALIAS qualifier), so §12.9 must contribute NOTHING.
//     An arm that answered a variance off the enclosing type regardless of
//     whether the nested one exists would add a second, spurious §12.9 to a
//     program already rejected — #6351's first attempt, which was REJECTED for
//     exactly that shape.
//   * row 7/8 do the same at THREE segments, and row 8 is what makes the
//     per-segment walk (rather than a two-part rule) load-bearing.
//   * row 9 pins the `None` qualifier: a qualifier naming nothing at all still
//     resolves nothing, so the new arm cannot become a fallback that guesses.
//
// DELIBERATELY NOT PINNED: a nested head named by its BARE name from inside the
// enclosing type's own body (`class Outer { interface Box<in T> … method M():
// Box<T>`) is still `Unknown` and still skipped. `localTypes` holds the module's
// top-level declarations, and a body-position bare name resolves through the
// enclosing type's scope, which this pass does not model. That is a separate
// residual, not this issue; no row here asserts it either way.

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

// `Outer` with one nested interface head, marker chosen by the caller.
std::string nested(const char* marker, const char* body) {
    return std::string("class Outer public interface Box<") + marker + " T> " +
           body + " end Box; end Outer; ";
}

// REJECT with §12.9, and with the polarity sentence the row is about.
void expectVariance129(const std::string& decls, const char* polarity) {
    auto r = runAot(program(decls));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, polarity));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kComposedClause));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kVariance129));
}

// REJECT on some OTHER rule, with §12.9 contributing nothing.
void expectRejectedWithout129(const std::string& decls, const char* other) {
    auto r = runAot(program(decls));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, other));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kVariance129));
}

// ACCEPT: the composition is sound, so the declaration must compile clean.
void expectAccepted(const std::string& decls) {
    auto r = runAot(program(decls));
    BEZEL_EXPECT_TRUE(r.ran);
}

// The interface under test: `out T` (or `in T`) composed through the head.
std::string iface(const char* marker) {
    return std::string("public interface I<") + marker +
           " T> method Get(): Outer.Box<T>; end I; ";
}

}  // namespace

// --- 1. the violation that used to compile ----------------------------------
//
// `Box<in T>` puts T in a CONTRAVARIANT slot; `Get()` is a RETURN slot, so T
// composes to contravariant, which `out T` cannot honour. On the base this
// compiled clean.

BEZEL_TEST(nested_local_head_variance_6369,
           contravariant_nested_head_under_out_return_rejected) {
    expectVariance129(nested("in", "method Put(x: T);") + iface("out"),
                      kOutInInput);
}

// --- 2. the mirror violation: `in T` through a COVARIANT head ---------------

BEZEL_TEST(nested_local_head_variance_6369,
           covariant_nested_head_under_in_return_rejected) {
    expectVariance129(nested("out", "method Get(): T;") + iface("in"),
                      kInOutput);
}

// --- 3. THE CONTROL that makes rows 1 and 2 discriminating ------------------
//
// Byte-identical to row 1 except the nested head's own polarity. Only the
// walk reading `Box<out T>`'s marker COVARIANT makes this compile; a walk that
// stopped at the enclosing `Outer` CLASS — a class is always invariant, §12.9 —
// would reject it. Row 1 alone cannot tell the two apart (#6373's review
// measured that mutant green: 12 passed, 0 failed).

BEZEL_TEST(nested_local_head_variance_6369,
           covariant_nested_head_under_out_return_accepted) {
    expectAccepted(nested("out", "method Get(): T;") + iface("out"));
}

// --- 4. the sound composition: contravariant-of-contravariant ---------------

BEZEL_TEST(nested_local_head_variance_6369,
           contravariant_nested_head_under_in_return_accepted) {
    expectAccepted(nested("in", "method Put(x: T);") + iface("in"));
}

// --- 5. a nested head that does not exist: §12.9 must say nothing -----------
//
// `resolveLocalNestedHead` walks `Outer`'s members and finds no `Nope`, so it
// answers nothing; `resolveNamed` reports the spelling itself. A §12.9 line
// here would be the false resolve that got #6351's first attempt rejected.

BEZEL_TEST(nested_local_head_variance_6369,
           missing_nested_member_adds_no_variance) {
    expectRejectedWithout129(
        nested("in", "method Put(x: T);") +
            "public interface I<out T> method Get(): Outer.Nope<T>; end I; ",
        "has no nested type named `Nope`");
}

// --- 6. a LOCAL ALIAS qualifier names no members at all ---------------------
//
// §2.7 rule 1 resolves `Outer` to the §27.8 alias, and an alias has no
// enclosing type, so `Outer.Box<T>` names nothing. #6373's `classifyHeadQualifier`
// answers `LocalAlias` and this arm must not fire.

BEZEL_TEST(nested_local_head_variance_6369,
           local_alias_qualifier_adds_no_variance) {
    expectRejectedWithout129(
        "type Outer := int; "
        "public interface I<out T> method Get(): Outer.Box<T>; end I; ",
        "generic-argument count does not match type alias `Outer`");
}

// --- 7. three segments: the walk is per-segment, not two-part ---------------

BEZEL_TEST(nested_local_head_variance_6369,
           deeply_nested_contravariant_head_under_out_return_rejected) {
    expectVariance129(
        "class Outer public class Mid public interface Deep<in T> "
        "method Put(x: T); end Deep; end Mid; end Outer; "
        "public interface I<out T> method Get(): Outer.Mid.Deep<T>; end I; ",
        kOutInInput);
}

// --- 8. and the same three-segment spelling, covariantly: ACCEPTED ----------
//
// This is what makes row 7 discriminating. A "stop at the enclosing class" walk
// reads `Mid` (a class, invariant) and rejects this; only a walk that reaches
// `Deep` answers COVARIANT and lets it compile.

BEZEL_TEST(nested_local_head_variance_6369,
           deeply_nested_covariant_head_under_out_return_accepted) {
    expectAccepted(
        "class Outer public class Mid public interface Deep<out T> "
        "method Get(): T; end Deep; end Mid; end Outer; "
        "public interface I<out T> method Get(): Outer.Mid.Deep<T>; end I; ");
}

// --- 9. a qualifier that names nothing stays unanswerable --------------------
//
// `classifyHeadQualifier` answers `None`; no arm may guess a variance for it.

BEZEL_TEST(nested_local_head_variance_6369,
           unresolved_qualifier_adds_no_variance) {
    expectRejectedWithout129(
        "public interface I<out T> method Get(): Undefined.Box<T>; end I; ",
        "unresolved type name `Undefined`");
}