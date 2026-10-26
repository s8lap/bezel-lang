// Spec-derived regression test for issue #7257 — the UNSPECIALIZED generic
// TEMPLATE body's dead local degraded SILENTLY, and the `internal:`
// diagnostic that used to expose the degrade is for LIVE lowerings.
//
// spec §14.1 "Generic declarations" (v0.344) — *Instantiation and its site*:
// an instantiation is "the constructed type or constructed member obtained by
// substituting type arguments for its type parameters", and its site is "the
// program point at which the program forms one".
// spec §3.6 rule 2 "Per-instantiation evaluation for generics" (v0.344): "A
// generic body type-checks under the weakest assumption its constraints
// establish; an instantiation that violates a property-dependent requirement
// is rejected at the instantiation site."
//
// WHAT THE DEFECT WAS. `Emitter::lowerLocalDecl`
// (src/codegen/LlvmBackend_Setup.cpp) has three outcomes for an initializer
// that produced no lowered value: (a) a UNIT-typed initializer (legitimate —
// `()` lowers to LLVM `void`), (b) a member call on a BUILT-IN receiver
// (#2935, a located Error), (c) EVERYTHING ELSE — the §3/#605 permissive
// surface, which #2434 made a `Severity::Warning` whose text is
//
//     internal: lowerLocalDecl's initializer expression failed to lower to a
//     value for this local's slot (degraded to a zero-initialized value) — #2434
//
// #2434's own words: "With the fallback in place, any *future* lowering
// failure on this path will now silently produce `0` instead of exhibiting a
// symptom, which is materially harder to find than either an ICE or a garbage
// read." ⇒ the diagnostic's purpose is to expose a failure that CAN MATTER.
//
// In the UNSPECIALIZED TEMPLATE body it cannot matter. A generic method's
// template body is written against the ABSTRACT type parameter: `x[0]` on a
// bare `T` has no indexer to lower to and `x.ToString()` on a bare `T` has no
// method to dispatch to, so the template's local ALWAYS degrades — while the
// SPECIALIZATION (what the live call actually binds) lowers the same
// initializer correctly. The corpus case that triggered the ratchet rise
// (tests/conformance/rules/cases/s14_1_0010.bzl) is exactly this shape: the
// program prints `1` and exits 0, and the warning it emitted was about code
// that never runs.
//
// THE FIX. The #2434 report in branch (c) is now gated on
// `!emittingGenericTemplateBody_` — the SAME gate #6757's ruling already
// established for the dead template's value in
// src/codegen/LlvmBackend_Exprs.cpp. The zero-initialized slot is NOT
// touched: the store is outside the branch, so the template's local is still
// deterministically zero-initialized.
//
// ── THE ORACLE (this is the part that needed care) ─────────────────────────
//
// ⚠ THE VACUOUS-ORACLE TRAP. `runAot` discards the compiler's stderr on a
// SUCCESSFUL compile (`CompileOutcome::Ok` drops the captured diagnostics),
// so a gtest that asserted "the program compiled" — or even that inspected
// `r.diagnostics` — would NOT see this Warning AT ALL, on either arm. Such a
// row is VACUOUS: it cannot fail without the fix.
//
// ⇒ THE DIAGNOSTIC ORACLE HERE IS AN IN-PROCESS FRONT-END + CODEGEN RUN
// (`codegen::emitModule` over the test's OWN `DiagnosticEngine`), the pattern
// tests/codegen/abstract_simple_form_property_no_storage_2939_test.cpp
// already uses. `DiagnosticEngine::report` records EVERY severity
// (src/support/Diagnostic.cpp), so the codegen's Warning lands in the test's
// engine and the row can assert its presence AND its absence. The AOT
// `runAot` oracle is used for the VALUE channel only.
//
// ROWS (all four are pinned here; the two controls are green on BOTH arms):
//
//   1. positive        the template's dead local ⇒ NO `internal:` diagnostic
//   2. negative ctrl   a NON-template local that fails to lower ⇒ the
//                      #2434 diagnostic is STILL emitted
//   3. value ctrl      the program still prints `1` and exits 0
//   4. scope ctrl      the LIVE SPECIALIZATION's own degrade ⇒ still exactly
//                      ONE #2434 diagnostic (the template's copy is gone, the
//                      live copy is not), and the degraded local is still
//                      zero-initialized (the program prints `0`)

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The exact #2434 needle. Matched as a SUBSTRING of the diagnostic message,
// so the row pins the diagnostic's identity, not its position.
const char* k2434 = "internal: lowerLocalDecl's initializer expression failed "
                    "to lower to a value for this local's slot";

// The result of an in-process front-end + codegen run, with the diagnostics
// the test can actually SEE (the driver's own engine is a local).
struct Compiled {
    bool rejected = false;        // the front end or codegen reported an Error
    std::vector<std::string> messages;
};

Compiled compileInProcess(const std::string& src) {
    Compiled out;
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) {
        out.rejected = true;
    } else {
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        if (!diags.hasErrors()) {
            auto llvmCtx = std::make_unique<llvm::LLVMContext>();
            (void)bezel::codegen::emitModule(*unit, ctx, *llvmCtx,
                                             bezel::codegen::CodegenOptions{});
        }
    }
    out.rejected = diags.hasErrors();
    for (const auto& d : diags.diagnostics())
        out.messages.push_back(d.message);
    return out;
}

int count2434(const Compiled& c) {
    int n = 0;
    for (const auto& m : c.messages)
        if (m.find(k2434) != std::string::npos) ++n;
    return n;
}

// Every diagnostic whose message opens with the `internal:` prefix — the
// convention the ratchet scripts/check_internal_residual.py counts.
int countInternal(const Compiled& c) {
    int n = 0;
    for (const auto& m : c.messages)
        if (m.rfind("internal:", 0) == 0) ++n;
    return n;
}

void dump(const Compiled& c) {
    std::fprintf(stderr, "  rejected=%d diagnostics (%zu):\n", (int)c.rejected,
                 c.messages.size());
    for (const auto& m : c.messages)
        std::fprintf(stderr, "    [%s]\n", m.c_str());
}

// ── the programs ───────────────────────────────────────────────────────────

// (1) the corpus shape (tests/conformance/rules/cases/s14_1_0010.bzl): a
// generic method whose TEMPLATE body indexes a bare `T`. The instantiation
// at `Array<int>` is what runs, and it lowers correctly.
const char* kTemplateProgram =
    "module M7257; "
    "public class Program "
    "  public static method Main() "
    "    var a: Array<int> := [1, 2, 3]; "
    "    Console.WriteLine(F<Array<int>>(a)); "
    "  end Main; "
    "  public static method F<T>(x: T): int "
    "    var v: int := x[0]; "
    "    return v; "
    "  end F; "
    "end Program; ";

// (2) the NEGATIVE CONTROL — byte-for-byte the source of
// tests/lit/local_decl_zero_init_fallback_warns_2434.bzl. A NON-generic
// method's local whose initializer is a member call on an `Object`-typed
// receiver: sema accepts it BY DESIGN (§3/#605 permissive "unknown member"),
// codegen has no member to dispatch to, so the initializer fails to lower and
// #2434's warning is the ONLY signal. It must keep firing.
const char* kNonTemplateProgram =
    "module M7257n; "
    "public class C "
    "  public method M(o: Object): int "
    "    var s := o.Anything(); "
    "    return 0; "
    "  end M; "
    "end C; ";

// (3) the SCOPE CONTROL — the SAME generic method, but instantiated at
// `int` IN THE SAME MODULE. Now the LIVE code is the SPECIALIZATION
// (`Program$F$spec`), and `x[0]` on an `int` has no indexer either, so the
// SPECIALIZATION's own local degrades too. That degrade is LIVE code and must
// keep its warning — the fix must silence the TEMPLATE's copy only.
const char* kSpecializationProgram =
    "module M7257s; "
    "public class Program "
    "  public static method Main() "
    "    Console.WriteLine(F<int>(42)); "
    "  end Main; "
    "  public static method F<T>(x: T): int "
    "    var v: int := x[0]; "
    "    return v; "
    "  end F; "
    "end Program; ";

}  // namespace

// ===========================================================================
// ROW 1 — POSITIVE: the generic TEMPLATE body's dead local must NOT emit the
// `internal:` diagnostic. Before the fix this program emitted exactly ONE
// `internal:` line (the template's #2434 warning); after the fix it emits
// NONE. The program is CORRECT on both arms — it prints `1` — which is
// precisely why the diagnostic was the wrong thing to emit.
// ===========================================================================
BEZEL_TEST(codegen_generic_template_local_degrade_7257,
           template_body_degraded_local_emits_no_internal_diagnostic) {
    Compiled c = compileInProcess(kTemplateProgram);
    if (c.rejected) dump(c);
    BEZEL_EXPECT_FALSE(c.rejected);
    // The exact needle: no #2434 report for the template's dead local.
    BEZEL_EXPECT_EQ(count2434(c), 0);
    // The stronger form: no `internal:` diagnostic AT ALL for this program.
    BEZEL_EXPECT_EQ(countInternal(c), 0);
}

// ===========================================================================
// ROW 3 — VALUE CONTROL: the fix must not change the answer. The
// specialization still reads `a[0]` and the program still prints `1`.
// GREEN ON BOTH ARMS (a control).
// ===========================================================================
BEZEL_TEST(codegen_generic_template_local_degrade_7257,
           template_body_degraded_local_still_prints_one) {
    JitResult r = runAot(kTemplateProgram);
    if (!r.ran) dump(compileInProcess(kTemplateProgram));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// ===========================================================================
// ROW 2 — NEGATIVE CONTROL: a NON-template local whose initializer fails to
// lower STILL emits the #2434 diagnostic. This is the third spelling the fix
// must not swallow: the gate is `emittingGenericTemplateBody_`, and this
// method is not a generic template at all.
// GREEN ON BOTH ARMS (a control).
// ===========================================================================
BEZEL_TEST(codegen_generic_template_local_degrade_7257,
           non_template_degraded_local_still_emits_internal_diagnostic) {
    Compiled c = compileInProcess(kNonTemplateProgram);
    if (c.rejected) dump(c);
    BEZEL_EXPECT_FALSE(c.rejected);
    BEZEL_EXPECT_EQ(count2434(c), 1);
    // TWO `internal:` diagnostics reach this program, and both must survive:
    // the #2477 dispatch note ("codegen cannot dispatch member 'Anything' on
    // receiver of type 'Object'") and the #2434 zero-init backstop that
    // follows from it. The diagnostic is a Warning, so the program still
    // compiles — #2434 made it a Warning precisely so the §3/#605 permissive
    // programs keep building.
    BEZEL_EXPECT_EQ(countInternal(c), 2);
}

// ===========================================================================
// ROW 4 — SCOPE CONTROL: the LIVE SPECIALIZATION's own degrade keeps its
// warning. Before the fix this program emitted TWO #2434 lines (the dead
// TEMPLATE's copy and the live SPECIALIZATION's copy); after the fix it
// emits exactly ONE — the live one. This row is what proves the fix did not
// silence the diagnostic wholesale: it silenced the DEAD copy and left the
// LIVE copy alone.
//
// It also pins the OTHER half of #2434: the degraded slot is STILL
// zero-initialized (the program prints `0`), so the fix did not trade the
// warning for an uninitialized read.
// ===========================================================================
BEZEL_TEST(codegen_generic_template_local_degrade_7257,
           live_specialization_degraded_local_still_emits_one_diagnostic) {
    Compiled c = compileInProcess(kSpecializationProgram);
    if (c.rejected) dump(c);
    BEZEL_EXPECT_FALSE(c.rejected);
    BEZEL_EXPECT_EQ(count2434(c), 1);
    // And it is the ONLY `internal:` diagnostic: the specialization's `x[0]`
    // on an `int` has no indexer to dispatch to, so it produces no #2477
    // dispatch note — only the #2434 backstop.
    BEZEL_EXPECT_EQ(countInternal(c), 1);

    // The zero-initialized slot survives the fix (#2434's other half).
    JitResult r = runAot(kSpecializationProgram);
    if (!r.ran) dump(c);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}
