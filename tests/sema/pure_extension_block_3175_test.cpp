// fix #3175 (§22.15 "Pure members", v0.254) — a `pure` method declared
// inside an `extension` block NEVER had its body checked against §22.15's
// pure-body rules. The identical body inside an ordinary class method was
// rejected.
//
// §22.15 (v0.254), verbatim: "The compiler rejects a `pure` member whose
// body violates the rules of this section, and rejects, inside a pure body,
// any call whose statically resolved declaration is not pure."
//
// §22.15 "Where `pure` may appear" (v0.254): "`pure` is admitted on:
// instance and static methods; **extension methods and expression-bodied
// extension properties**; operator declarations; constructors; properties,
// indexers, and `get` accessors; ..."
//
// §22.15 "The pure body" rule 2: assignment is admitted only where "the
// target's root is a local variable or by-value parameter of this body and
// the access path from that root traverses only value-typed storage". The
// contrapositive: any store whose target root leaves the frame is rejected,
// including a static field.
//
// WHAT THE DEFECT WAS. `src/sema/PureChecker.cpp:checkPureBodies` walked
// `ctx.module()->children` for `TypeSymbol`s and visited their members —
// extension blocks are `ExtensionSymbol`s, which live at module scope, NOT
// under a `TypeSymbol`'s members (§A.3, SymbolCollection.cpp:439; Symbol.hpp:
// 793). The pre-fix code carried a comment asserting the extension walk
// existed "separately below"; no such loop existed. `grep -n Extension
// src/sema/PureChecker.cpp` returned exactly that comment and nothing else.
// An extension method's body was never reached by the pure checker, and the
// import-side artifact (`ModuleMetadataBuilder.cpp:870`) writes the
// `isPure` bit from the member's modifiers with no body check behind it, so
// the false `pure` laundered arbitrary effects into every context that
// trusts the `pure` marker.
//
// Measured pre-fix on `b2e1bf3` (`build/bezelc -o m8.exe m8.local.bzl`):
//
//   m8.local.bzl  — `pure` body inside an `extension int` block, writes a
//                   static field (`SinkM8.Count := ...`). EXIT 0 — false
//                   accept.
//   m8b.local.bzl — the SAME statement inside a `pure` class method. EXIT 1,
//                   diagnostic: "assignment is not admitted in a pure body
//                   — the target root is not a frame-local of this body
//                   (§22.15)".
//
// The fix routes extension members through the same `checkOneMember` path
// used for class members, after the type-symbol loop. The comment that
// asserted coverage is removed (a comment that asserts coverage next to
// newly-added coverage is how it drifts again, per the issue's "fix shape").
//
// ⚠ THE OVER-REJECTION CONTROL IS HALF THE TEST. A test set that only pinned
// what must be rejected would pass on a compiler that rejected every pure
// extension method. T2 — a `pure` extension method with a pure body — must
// still COMPILE. It is the row that fails if the new walk is over-broad.

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
        bezel::sema::checkPureBodies(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// The §22.15 static-field-assignment sentence, as the diagnostic renders it.
const char* kStaticStoreMsg =
    "assignment is not admitted in a pure body";

}  // namespace

// ===========================================================================
// T1 — §22.15 SOUNDNESS: a `pure` extension method with an impure body
// (writes a static field) MUST be REJECTED with the §22.15 diagnostic.
//
// This is the issue's headline row. The twin statement inside an ordinary
// `pure` class method is rejected with the SAME diagnostic text (the
// single-module twin tested in pure_body_22_15_test.cpp:
// pure_writes_static_field_rejected_spec_22_15). Pre-fix: the extension
// form was accepted with exit 0; post-fix: it MUST be rejected identically.
// ===========================================================================
BEZEL_TEST(pure_extension_block_3175,
           t1_pure_extension_method_writes_static_rejected_22_15) {
    auto r = parseSemaAll(
        "module M3175T1; "
        "public class Sink "
        "  private static _s: int := 0; "
        "end Sink; "
        "public extension int "
        "  public pure method Tainted(): int "
        "    Sink._s := Sink._s + 1; "
        "    return 1; "
        "  end Tainted; "
        "end; "
        "public class C "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, kStaticStoreMsg));
    BEZEL_EXPECT_TRUE(hasMsg(r, "22.15"));
}

// ===========================================================================
// T1b — SINGLE-MODULE TWIN of T1: the SAME statement inside a `pure` class
// method is REJECTED with the SAME diagnostic. The differential control that
// makes T1 a §22.15 claim rather than a parser/extension-typing claim.
// ===========================================================================
BEZEL_TEST(pure_extension_block_3175,
           t1b_pure_class_method_writes_static_rejected_22_15) {
    auto r = parseSemaAll(
        "module M3175T1b; "
        "public class Sink "
        "  private static _s: int := 0; "
        "end Sink; "
        "public class C "
        "  public pure method Tainted(): int "
        "    Sink._s := Sink._s + 1; "
        "    return 1; "
        "  end Tainted; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, kStaticStoreMsg));
    BEZEL_EXPECT_TRUE(hasMsg(r, "22.15"));
}

// ===========================================================================
// T2 — OVER-REJECTION CONTROL: a `pure` extension method with a PURE body
// (frame-local operations only) MUST still compile.
//
// A regression to "always reject pure extension methods" would break this
// row; a regression to "always accept" would break T1/T1b. Together T1 and
// T2 pin the §22.15 boundary.
// ===========================================================================
BEZEL_TEST(pure_extension_block_3175,
           t2_pure_extension_method_pure_body_admitted_22_15) {
    auto r = parseSemaAll(
        "module M3175T2; "
        "public extension int "
        "  public pure method Doubled(): int "
        "    var x: int := this; "
        "    return x + x; "
        "  end Doubled; "
        "end; "
        "public class C "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// T3 — §22.15 "expression-bodied extension properties": a `pure` extension
// property (expression-bodied, the only form §26.3 admits in extensions)
// must also be walked by the pure checker (per the issue's blast radius
// and the §22.15 "Where `pure` may appear" list). A `pure` property whose
// expression body calls a non-pure method is rejected by §22.15 rule 6
// (calls to non-pure members).
//
// This is the SECOND covered member form (the fix walks all `MemberSymbol`s
// of every `ExtensionSymbol`; the property case is the non-method arm —
// `checkOneMember`'s `PropertySymbol*` branch). The block-accessor form is
// rejected at parse time by §26.3 (the diagnostic "extension properties
// must be expression-bodied…"), so only the expression-bodied path can
// reach the pure checker.
// ===========================================================================
BEZEL_TEST(pure_extension_block_3175,
           t3_pure_extension_property_calls_impure_rejected_22_15) {
    auto r = parseSemaAll(
        "module M3175T3; "
        "public class Sink "
        "  public static method Impure(): int "
        "    var x: int := 0; "
        "    return x; "
        "  end Impure; "
        "end Sink; "
        "public extension int "
        "  public pure property Tainted: int => Sink.Impure(); "
        "end; "
        "public class C "
        "  public static method Main(): int => 0; "
        "end C;");
    // Diagnostic inspection: print whatever was reported so a regression is
    // legible without re-running.
    if (!r.diagnostics.hasErrors() ||
        !hasMsg(r, "not pure-declared")) {
        for (const auto& d : r.diagnostics.diagnostics()) {
            std::fprintf(stderr, "  diag: sev=%d msg=%s\n",
                         static_cast<int>(d.severity), d.message.c_str());
        }
    }
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "not pure-declared"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "22.15"));
}
