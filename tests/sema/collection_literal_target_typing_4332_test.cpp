// fix #4332 / spec §15.4 *Collection literals* (v0.298).
//
// `fix #4310` (tests/sema/collection_literal_element_interface_conformance_
// 4310_test.cpp) threaded target-typing onto a collection literal at six
// positions: LocalDecl, LocalConstDecl, FieldDecl, PropertyDecl auto-
// initializer, ReturnStmt, and the general call/constructor-argument path.
// #4332 identifies FOUR further positions that never set the literal's
// own `exprType` and therefore never run the per-element assignability walk:
//   1. Default parameter value
//   2. Yield return
//   3. Case-expression arm body
//   4. `as` / TypeTestExpr operand
//
// ROOT CAUSE. Each position's handler sets no `exprType` on a bare
// CollectionLiteralExpr before visiting it. Without `exprType` set,
// `ctx_.exprType(*cl)` is null, `checkCollectionLiteralElementAssignability`
// skips its element walk (its `if (!from) return` guard), and a non-conforming
// element is silently accepted. The symptom: compiles clean, then SIGSEGVs at
// the interface dispatch on the first element read.
//
// THE FIX threads the resolved target type onto `cl->exprType` before visiting
// the literal, then calls `checkBorrowingFamilyLiteralElements` and
// `checkCollectionLiteralElementAssignability` — the same wiring as every
// other #4310-family position. §15.4 admissibility is gated first; per-
// element assignability follows.
//
// Co-Authored-By: MiniMax-M3 <noreply@anthropic.com>

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §5.1 diagnostic: "has a type that is not implicitly convertible to the
// target type (§5.1)" — same authority checkAssignable produces for a plain
// variable declaration; this fix calls the identical function per element.
bool hasNotImplicitlyConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

// Shared prelude: IHolder interface, a NON-conforming Widget3 (no inheritance
// clause), and a conforming Widget3Ok.
const std::string kPrelude =
    "module M; import System.Collections.Generic; "
    "interface IHolder method GetV(): string; end IHolder; "
    "class Widget3 "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3; "
    "class Widget3Ok: IHolder "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3Ok; ";

}  // namespace

// =====================================================================
// POSITION 1 — default parameter value
// =====================================================================
// Without fix: compiles clean, SIGSEGVs at h.GetV() inside callee.
// With fix: rejects with §5.1 "not implicitly convertible".

BEZEL_TEST(collection_literal_target_typing_4332,
           default_param_value_noconform_rejected_spec_15_4) {
    // The default value is [new Widget3(s)] where Widget3 does NOT implement
    // IHolder, but the parameter is typed Array<IHolder>. Pre-fix the literal
    // had no exprType and checkCollectionLiteralElementAssignability skipped;
    // with the fix, Widget3's lack of IHolder membership is caught per §5.1.
    JitResult r = runAot(kPrelude +
        "class C "
        "  public static method TakesIt(s: string, x: Array<IHolder> := [new Widget3(s)]): string "
        "    return x[0].GetV(); "
        "  end TakesIt; "
        "  public static method Main(): int "
        "    return C.TakesIt(\"hello\").Length; "
        "  end Main; "
        "end C;");
    // Without fix: r.ran == true, SIGSEGV exit; with fix: rejected.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// =====================================================================
// POSITION 4 — `as` / TypeTestExpr operand
// =====================================================================
// Without fix: compiles clean, SIGSEGVs at h.GetV() on the cast result.
// With fix: rejects with §5.1 "not implicitly convertible".

BEZEL_TEST(collection_literal_target_typing_4332,
           as_operand_noconform_rejected_spec_15_4) {
    JitResult r = runAot(kPrelude +
        "class C "
        "  public static method Main(): int "
        "    var arr := [new Widget3(\"hi\")] as Array<IHolder>; "
        "    return arr[0].GetV().Length; "
        "  end Main; "
        "end C;");
    // Without fix: r.ran == true, SIGSEGV exit; with fix: rejected.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// =====================================================================
// POSITION 2 — yield return
// =====================================================================
// Without fix: compiles clean, SIGSEGVs when the first yielded element is
// dispatched through the IHolder interface.
// With fix: rejects with §5.1 "not implicitly convertible".

BEZEL_TEST(collection_literal_target_typing_4332,
           yield_return_noconform_rejected_spec_15_4) {
    // Widget3 does NOT implement IHolder; the method returns IEnumerable<IHolder>.
    // Pre-fix the literal had no exprType and the element conformance check
    // was skipped; with the fix, Widget3's lack of IHolder membership is
    // caught per §5.1 via a direct checkAssignable walk over the literal's
    // elements against the iterator's element type (linqSourceElementType).
    JitResult r = runAot(kPrelude +
        "class C "
        "  public method Iter(): IEnumerable<IHolder> "
        "    yield return [new Widget3(\"hi\")]; "
        "  end Iter; "
        "  public static method Main(): int "
        "    return 1; "
        "  end Main; "
        "end C;");
    // Without fix: r.ran == true, SIGSEGV exit; with fix: rejected.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
