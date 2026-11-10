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

}  // namespace

// ── §21.9 / stdlib §7.3: the issue's repro compiles with NO unused-result warning ──

BEZEL_TEST(file_delete_8389, delete_statement_no_unused_result_warning_spec_21_9) {
    auto r = semaAll(
        "module M; "
        "class C public static method Main(): int "
        "  File.Delete(\"/tmp/bzt8389.txt\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(unusedResultWarnings(r), 0);
}

// ── THE CONTROL: `File.Exists` DOES return a `bool`, so the §21.9 regime is
//    unchanged for it — the fix narrows `File.Delete` alone and does not
//    silence the section. ──

BEZEL_TEST(file_delete_8389, exists_statement_still_draws_the_unused_result_warning) {
    auto r = semaAll(
        "module M; "
        "class C public static method Main(): int "
        "  File.Exists(\"/tmp/bzt8389.txt\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(unusedResultWarnings(r), 1);
}

// ── THE CONTROL: the sibling `Directory.Delete` (§7.4) already declared no
//    result and already drew no warning; it must be unchanged. ──

BEZEL_TEST(file_delete_8389, directory_delete_statement_is_unchanged) {
    auto r = semaAll(
        "module M; "
        "class C public static method Main(): int "
        "  Directory.Delete(\"/tmp/bzt8389\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(unusedResultWarnings(r), 0);
}
