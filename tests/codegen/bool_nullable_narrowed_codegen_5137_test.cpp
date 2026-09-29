// fix #5137 (§7.11, v0.313): the §7.11 'no three-valued logic' gate at
// and/or/not/if-condition/while-condition/if-expression-condition reads
// the operand's NARROWED static type (not the declared type), and codegen
// extracts the `i1` payload from the {i8 hasVal; i1 payload} storage when
// the sema gate has admitted the operand. Before this fix, every program
// here ICE'd or SEGV'd at codegen because the sema gate rejected narrowed
// reads (forcing codegen to be unreachable for them), so the codegen
// extraction was never tested. This file pins the four accept rows the
// issue's repros demand; the four reject rows live in
// tests/lit/sema/sema_lifted_ops_486* and tests/lit/sema/sema_cond__r39*
// and stay reject (sema half of #5137 preserved their diagnostics).

#include "support/aot_support.hpp"   // runAot
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// runAot returns JitResult {ran, exitCode, stdoutText, diagnostics}; the
// program must print "ok\n" to stdout (per §22.13 — the value channel for
// observables that don't fit an 8-bit exit code).
int runAndCheck(const std::string& body, const std::string& want,
                std::string* outDiag = nullptr) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    if (!r.ran) {
        if (outDiag) {
            std::string s = "did not run";
            for (const auto& d : r.diagnostics.diagnostics()) {
                if (d.severity == bezel::Severity::Error) {
                    s += " | err@";
                    s += std::to_string(d.location.line);
                    s += ":";
                    s += std::to_string(d.location.column);
                    s += " ";
                    s += d.message;
                }
            }
            *outDiag = s;
        }
        return -1;
    }
    if (r.exitCode != 0) {
        if (outDiag) *outDiag = "exit=" + std::to_string(r.exitCode);
        return -2;
    }
    if (r.stdoutText.find(want) == std::string::npos) {
        if (outDiag) {
            *outDiag = "stdout=\"" + r.stdoutText + "\"";
        }
        return -3;
    }
    return 0;
}

} // namespace

// fix #5137 — Probe A: `var b: bool? := true; if b and true then … end`.
// The initializer narrows b to non-null at the `and`, so §7.11 admits
// the operand; codegen must extract the i1 from the {i8, i1} storage
// to branch on it.
BEZEL_TEST(codegen_bool_nullable_5137, narrowed_and_init_runs_true_branch) {
    BEZEL_EXPECT_EQ(runAndCheck(
        "var b: bool? := true; "
        "if b and true then Console.WriteLine(\"A\"); end; "
        "return 0;",
        "A\n"),
        0);
}

// fix #5137 — Probe B: nested `b <> null` then `not b`. The outer null
// check narrows b non-null at the `not`; sema admits the operand; codegen
// must extract the i1 to feed `CreateNot`.
BEZEL_TEST(codegen_bool_nullable_5137, narrowed_not_after_null_check_runs) {
    std::string diag;
    int rc = runAndCheck(
        "var b: bool?; b := false; "
        "if b <> null then if not b then Console.WriteLine(\"B\"); end; end; "
        "return 0;",
        "B\n", &diag);
    BEZEL_EXPECT_EQ(rc, 0);
    if (rc != 0) {
        // Surface diagnostic detail even when rc != 0.
        fprintf(stderr, "Probe B rc=%d %s\n", rc, diag.c_str());
    }
}

// fix #5137 — Probe G: nested `b <> null` then bare `if b`. The outer
// null check narrows b non-null at the inner `if`; sema's
// checkConditionIsBool admits the condition; codegen must extract the
// i1 to feed CreateCondBr.
BEZEL_TEST(codegen_bool_nullable_5137, narrowed_if_after_null_check_runs) {
    std::string diag;
    int rc = runAndCheck(
        "var b: bool?; b := true; "
        "if b <> null then if b then Console.WriteLine(\"G\"); end; end; "
        "return 0;",
        "G\n", &diag);
    BEZEL_EXPECT_EQ(rc, 0);
    if (rc != 0) {
        fprintf(stderr, "Probe G rc=%d %s\n", rc, diag.c_str());
    }
}

// fix #5137 — Probe G2: composite `b <> null and b`. The left operand of
// `and` is the explicit `b <> null` null check (i1); the RIGHT operand
// is `b`, narrowed non-null by the left, and sema's §7.11 gate admits
// it. Codegen must extract the i1 from b for both the right-branch
// condition and the PHI at `sc.end`.
BEZEL_TEST(codegen_bool_nullable_5137, narrowed_and_rhs_runs) {
    BEZEL_EXPECT_EQ(runAndCheck(
        "var b: bool? := true; "
        "if b <> null and b then Console.WriteLine(\"G2\"); end; "
        "return 0;",
        "G2\n"),
        0);
}
