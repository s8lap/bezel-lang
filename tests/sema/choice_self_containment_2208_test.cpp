// Spec-derived sema test for #2208 (§35.1 + §9, v0.226): a value-typed
// component cycle that spans a record and a choice (record↔choice by value)
// is rejected at resolution time, before codegen. The repro on `main` a7881f3
// sent `Emitter::recordStruct` into unbounded LLVM struct-body recursion
// (a hang / stack overflow / coredump); the fix walks both record fields
// AND choice case payloads through the same value-containment graph
// (Resolver::recordValueContainsSelf, broadened), gated by `seen` so the
// walk terminates on malformed recursive graphs. The choice-side
// check (Resolver::checkChoiceSelfContainment) fires only on purely
// choice↔choice cycles — record↔choice cycles are reported exactly once,
// by the record-side check, so no duplicate diagnostics on a single
// malformed graph.

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;

namespace {

// fix #3985: delegates to the shared gated helper (sema_support.hpp)
// instead of duplicating the parse-error-gate logic locally.
bezel::test::SemaResult semaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

bool hasErrDiagWith(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

int countErrDiagWith(const bezel::test::SemaResult& r,
                     std::string_view needle) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            ++n;
    }
    return n;
}

}  // namespace

// --- #2208 REJECT: record↔choice by-value cycle (the original repro) -------

// §35.1 / §9: a record whose field is a choice whose payload carries the
// record back by value is a value-cycle. Without the resolution-time
// rejection the cycle sends Emitter::recordStruct into infinite LLVM
// struct-body recursion (#2208). The diagnostic fires on the record
// (the record-side check is the canonical site for record↔choice cycles;
// the choice check, keys recordsAllowed = false, deliberately does NOT
// fire here — this test asserts that single-diagnostic discipline).
BEZEL_TEST(choice_self_containment_2208,
           spec_35_1_v226_record_choice_cycle_record_side_single_diagnostic) {
    auto r = semaAll(
        "module M; "
        "record Rec(K: Ch); "
        "choice Ch Wrap(r: Rec), Leaf; end Ch; "
        "class C public static method Main(): int return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r,
        "record `Rec` cannot recursively contain itself by value"));
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r,
        "choice `Ch` cannot recursively contain itself by value"));
    // Record-side diagnostic fires EXACTLY once (no duplicate).
    BEZEL_EXPECT_EQ(countErrDiagWith(r,
        "record `Rec` cannot recursively contain itself by value"), 1);
}

// --- #2208 REJECT: purely choice↔choice cycle (the §35.1 cycle) ------------

// §35.1: a choice may not transitively contain itself through value-typed
// payload components. The walker (recordsAllowed = false) keys the choice
// check to PURELY choice↔choice cycles, so the diagnostic fires on the
// choice's own declLoc — never on a record that participates in the cycle.
// Repro: `choice A Wrap(b: B), Leaf` + `choice B Wrap(a: A), Leaf` — each
// starting choice independently violates §35.1 from its own declLoc, so
// both A AND B fire diagnostics (same shape as the existing §9 mutual
// record pair on the record side).
BEZEL_TEST(choice_self_containment_2208,
           spec_35_1_v226_pure_choice_pair_choice_side_diagnostic) {
    auto r = semaAll(
        "module M; "
        "choice A Wrap(b: B), Leaf; end A; "
        "choice B Wrap(a: A), Leaf; end B; "
        "class C public static method Main(): int return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r,
        "choice `A` cannot recursively contain itself by value through "
        "its case payloads"));
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r,
        "choice `B` cannot recursively contain itself by value through "
        "its case payloads"));
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r,
        "record `A` cannot recursively contain itself by value"));
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r,
        "record `B` cannot recursively contain itself by value"));
}

// --- #2208 ACCEPT: ordinary record→class (heap reference breaks the cycle) --

// §9: a record whose field is a class is a HEAP reference — the class breaks
// the value-typed cycle. The record side and the choice side both stay
// quiet. This test is the non-cyclic positive control: a record that
// holds a class that itself holds a choice-payload-carrying-record is
// not a value cycle.
BEZEL_TEST(choice_self_containment_2208,
           spec_9_v226_record_with_class_field_accepted_no_value_cycle) {
    // fix #3985: the constructor's block body was closed with `end Box;`,
    // but ConstructorDecl's own closer (Appendix A) is `"end" "constructor"?
    // ";"` — the optional word is literally `constructor`, not the
    // enclosing type's name (that trailer belongs to ClassBody's `"end"
    // Identifier? ";"` instead, one level up). `end Box;` therefore closed
    // nothing the parser expected here and the row never parsed, reaching
    // sema on semaAll's ungated, error-recovery-truncated AST. VERIFIED
    // against the real `bezelc -c`: `end constructor;` parses cleanly.
    auto r = semaAll(
        "module M; "
        "record Inner(X: int); "
        "class Box public i: Inner; public constructor(i: Inner) this.i := i; "
        "end constructor; "
        "end Box; "
        "record Outer(B: Box); "
        "class C public static method Main(): int return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r,
        "cannot recursively contain itself by value"));
}

// --- #2208 ACCEPT: non-cyclic record→choice (one-way, no payload cycle) -----

// §35.1: a record whose field is a non-recursive choice is fine. The
// choice's payload fields are all primitives — no value-typed component
// points back to the record. This is the non-cyclic record→choice control,
// the kind of layout that #2208's repro graph is trying to simulate.
BEZEL_TEST(choice_self_containment_2208,
           spec_35_1_v226_record_with_nonrecursive_choice_field_accepted) {
    auto r = semaAll(
        "module M; "
        "choice Color Red, Green(value: int), Blue; end Color; "
        "record Wrap(C: Color); "
        "class C public static method Main(): int return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r,
        "cannot recursively contain itself by value"));
}

// --- #2208 ACCEPT: non-cyclic choice→choice (one-way, no payload cycle) -----

// §35.1: a choice whose payload is a non-recursive choice is fine. This
// is the non-cyclic choice→choice control — the rejection must NOT fire
// here even though the data flow is choice→choice.
BEZEL_TEST(choice_self_containment_2208,
           spec_35_1_v226_choice_with_nonrecursive_choice_payload_accepted) {
    auto r = semaAll(
        "module M; "
        "choice Inner One, Two(value: int); end Inner; "
        "choice Outer Wrap(i: Inner), Leaf; end Outer; "
        "class C public static method Main(): int return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r,
        "cannot recursively contain itself by value"));
}

// --- #2208 REJECT: nested inline-array-of-record-of-choice-of-record cycle -

// §15.1 / §35.1 / §9: an inline array of a record that contains a choice
// that contains the original record is still a value cycle (the inline
// array is a fixed-size value struct, so its element participates in the
// value-containment graph). The unwrap step strips the inline-array.
//
// Repro: `record Rec(A: [3]Mid)` + `choice Mid Wrap(r: Rec), Leaf`.
BEZEL_TEST(choice_self_containment_2208,
           spec_35_1_v226_inline_array_of_record_of_choice_of_record_rejected) {
    auto r = semaAll(
        "module M; "
        "record Rec(A: [3]Mid); "
        "choice Mid Wrap(r: Rec), Leaf; end Mid; "
        "class C public static method Main(): int return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrDiagWith(r,
        "record `Rec` cannot recursively contain itself by value"));
    BEZEL_EXPECT_FALSE(hasErrDiagWith(r,
        "choice `Mid` cannot recursively contain itself by value"));
}
