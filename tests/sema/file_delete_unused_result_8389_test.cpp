// Spec-derived regression test for issue #8389.
//
// AUTHORITY. stdlib §7.3 (v0.355) declares
//
//     public static method Delete(path: string);   // no result
//     public static method Exists(path: string): bool;
//
// so `File.Delete` returns `()` — it produces NO value. spec §21.9 (v0.355):
// "An expression statement whose result is a non-`()` type is permitted but the
// compiler emits a warning: the result is silently discarded …" and "Method
// calls returning `()` and `await` expressions on `Task` (no result) are the
// common cases; no warning is emitted for these."
//
// THE DEFECT. The `File` arm of the method-typing block in
// ExprTypecheck_Calls.cpp typed `Exists` AND `Delete` as `Bool`, so the
// statement form `File.Delete(p);` drew the §21.9 unused-result warning. The
// sibling heads agree with the declaration, not with the arm: stdlib §7.4
// declares `Directory.Delete(path: string, recursive: bool := false)` — also
// no result — and the `Directory` arm leaves it untyped, so
// `Directory.Delete(p);` drew NO warning; `File.WriteAllText` /
// `AppendAllText` / `Copy` / `Move` are left untyped here for the same reason.
//
// The result is read through the SemaResult harness, which keeps warnings on
// successful compiles (runAot fills diagnostics only on reject).

#include "support/sema_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

namespace {

// Number of §21.9 unused-result warnings in the diagnostics list.
int unusedResultWarnings(const bezel::test::SemaResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("the result of this expression statement is unused") !=
                std::string::npos)
            ++n;
    }
    return n;
}

bezel::test::SemaResult semaAll(const std::string& src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// #5230: the paths these programs name must NOT be FIXED. `@DATA@` splices
// to this process's data directory under wtScratchDir() (unique_stem: pid +
// counter), removed at process exit and reaped if the process is SIGKILLed —
// a fixed temp path would both break the scratch policy (CLAUDE.md: scratch
// never lives in the tmpfs, which is RAM with a quota) and race with a
// concurrent test process on a shared machine. The programs below are only
// parsed and typechecked (they are never run), but the path they name is a
// real per-process path either way, so the fixture stays honest if it ever
// grows a run.
std::string prog(const std::string& body) {
    return bezel::test::spliceDataPaths(
        std::string("module M; "
                    "class C public static method Main(): int ") +
            body +
            "  return 0; "
            "end Main; end C;",
        "bzt8389");
}

}  // namespace

// ── §21.9 / stdlib §7.3: the issue's repro compiles with NO unused-result warning ──

BEZEL_TEST(file_delete_8389, delete_statement_no_unused_result_warning_spec_21_9) {
    const std::string src = prog("  File.Delete(\"@DATA@/bzt8389.txt\"); ");
    auto r = semaAll(src);
    BEZEL_EXPECT_EQ(unusedResultWarnings(r), 0);
}

// ── THE CONTROL: `File.Exists` DOES return a `bool`, so the §21.9 regime is
//    unchanged for it — the fix narrows `File.Delete` alone and does not
//    silence the section. ──

BEZEL_TEST(file_delete_8389, exists_statement_still_draws_the_unused_result_warning) {
    const std::string src = prog("  File.Exists(\"@DATA@/bzt8389.txt\"); ");
    auto r = semaAll(src);
    BEZEL_EXPECT_EQ(unusedResultWarnings(r), 1);
}

// ── THE CONTROL: the sibling `Directory.Delete` (§7.4) already declared no
//    result and already drew no warning; it must be unchanged. ──

BEZEL_TEST(file_delete_8389, directory_delete_statement_is_unchanged) {
    const std::string src = prog("  Directory.Delete(\"@DATA@/bzt8389\"); ");
    auto r = semaAll(src);
    BEZEL_EXPECT_EQ(unusedResultWarnings(r), 0);
}
