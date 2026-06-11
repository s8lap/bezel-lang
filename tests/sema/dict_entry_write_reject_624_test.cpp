// Spec-derived tests for issue #624 — writes to DictionaryEntry<K,V>
// positional-record fields (e.Key := …, e.Value := …, e.Value += …) must be
// rejected with a diagnostic per §9.4 (read-only positional fields).
//
// Root cause: the #548 synthetic DictionaryEntry loop-variable binding typed
// Key/Value reads but never checked assignability; the assignment went through
// the regular readonly-field path which only covers `this.<field>` shapes.
// Fix: in the AssignmentStmt handler, detect a MemberAccessExpr target whose
// receiver type has importedName == "DictionaryEntry" (the #548 tag) and
// emit the §9.4 diagnostic before the regular checks.
//
// per §9.4 (positional record field read-only), §4.8 v0.135, issue #624.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool anyDiagContains(const bezel::test::SemaResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(std::string(needle)) != std::string::npos) return true;
    return false;
}

// Helper: wrap body in a module/class/method that has a dict parameter.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; class C "
        "public method M(d: Dictionary<string, int>): int ") +
        body + " end M; end C;";
}

}  // namespace

// §9.4 (#624): `e.Value := x` inside a for-entry loop must be rejected with
// a read-only diagnostic.  DictionaryEntry.Value is a positional field.
BEZEL_TEST(sema_dict_entry_write_624, value_assign_rejected_spec_9_4) {
    auto r = parseSemaAll(wrap(
        "for var e in d do e.Value := 99; end; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "read-only"));
}

// §9.4 (#624): `e.Key := x` must also be rejected — Key is a positional
// read-only field just like Value.
BEZEL_TEST(sema_dict_entry_write_624, key_assign_rejected_spec_9_4) {
    auto r = parseSemaAll(wrap(
        "for var e in d do e.Key := \"new\"; end; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "read-only"));
}

// §9.4 (#624): compound assignment `e.Value += x` must also be rejected —
// any AssignOp (not just `:=`) is a mutation.
BEZEL_TEST(sema_dict_entry_write_624, value_compound_assign_rejected_spec_9_4) {
    auto r = parseSemaAll(wrap(
        "for var e in d do e.Value += 1; end; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "read-only"));
}

// §9.4 (#624): reads of e.Value must still be accepted — only writes are
// forbidden.
BEZEL_TEST(sema_dict_entry_write_624, value_read_still_accepted_spec_9_4) {
    auto r = parseSemaAll(wrap(
        "var n := 0; for var e in d do n := n + e.Value; end; return n;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.4 (#624): reads of e.Key must still be accepted.
BEZEL_TEST(sema_dict_entry_write_624, key_read_still_accepted_spec_9_4) {
    auto r = parseSemaAll(wrap(
        "var n := 0; "
        "for var e in d do var k: string := e.Key; n := n + 1; end; "
        "return n;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.4 (#624): the diagnostic must mention the field name (`Value` or `Key`).
BEZEL_TEST(sema_dict_entry_write_624, diagnostic_names_field_spec_9_4) {
    auto rv = parseSemaAll(wrap(
        "for var e in d do e.Value := 0; end; return 0;"));
    BEZEL_EXPECT_TRUE(anyDiagContains(rv, "Value"));
    auto rk = parseSemaAll(wrap(
        "var s: string := \"x\"; "
        "for var e in d do e.Key := s; end; return 0;"));
    BEZEL_EXPECT_TRUE(anyDiagContains(rk, "Key"));
}
