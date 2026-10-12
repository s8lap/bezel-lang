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
//
// #3563 follow-up (v0.334): every row above drives the SYNTHETIC carrier —
// the `for var e in d` element type, which carries the
// `importedName="DictionaryEntry"` tag. 38b32b9c widened the `.Key`/`.Value`
// TYPING guard (ExprTypecheck_Calls.cpp) to `headName()` so the WRITTEN
// annotation's carrier — `var e: IEnumerable<DictionaryEntry<string, V>> := d;`
// followed by `for var entry in e do` (§29.2's wrap face), which carries an
// astNode and an EMPTY importedName — became typed too. The §9.4 REJECTION
// and the #2444 closed-surface rejection here were NOT widened with it, so
// both went SILENT for exactly the spelling #3563 had just made reachable.
// The rows below pin the written carrier for both, and keep the synthetic
// carrier as an explicit differential control (a rejection that only ever
// fired for one carrier is not a fix).

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

// Helper: the same body, but the loop variable's DictionaryEntry<K,V> arrives
// through the WRITTEN annotation (§29.2's wrap face) instead of the synthetic
// `for var e in d` element type. `wrap`/`wrapWritten` differ ONLY in the
// receiver's carrier, so a row that flips between them is measuring the
// carrier and nothing else.
std::string wrapWritten(const std::string& body) {
    return std::string(
        "module M; class C "
        "public method M(d: Dictionary<string, int>): int "
        "var e: IEnumerable<DictionaryEntry<string, int>> := d; ") +
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

// ---------------------------------------------------------------------------
// #3563 follow-up (v0.334): the WRITTEN-annotation carrier.
//
// FAIL-WITHOUT (the sibling guard reverted to `importedName`): every
// WRITTEN-carrier row below compiled with NO error diagnostic — only an
// internal warning ("assignment target lvalue address failed to resolve
// (store dropped ...)" for the writes, "lowerExpr returned null for
// interpolated-string expression part" for the bogus member). MEASURED on the
// base compiler at exit 0 with an object file emitted.
// ---------------------------------------------------------------------------

// §9.4 (v0.334): `entry.Value := …` must be rejected when the receiver's
// DictionaryEntry<string,V> arrives through the WRITTEN annotation. This is
// the #3563 regression row: pre-fix this was a SILENT ACCEPT.
BEZEL_TEST(sema_dict_entry_write_624,
           written_spelling_value_assign_rejected_spec_9_4) {
    auto r = parseSemaAll(wrapWritten(
        "for var entry in e do entry.Value := 99; end; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "read-only positional fields"));
}

// §9.4 (v0.334): `entry.Key := …` through the written annotation — Key is a
// positional read-only field just like Value.
BEZEL_TEST(sema_dict_entry_write_624,
           written_spelling_key_assign_rejected_spec_9_4) {
    auto r = parseSemaAll(wrapWritten(
        "for var entry in e do entry.Key := \"new\"; end; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "read-only positional fields"));
}

// §9.4 (v0.334): compound assignment through the written annotation is the
// same mutation and must be rejected too.
BEZEL_TEST(sema_dict_entry_write_624,
           written_spelling_value_compound_assign_rejected_spec_9_4) {
    auto r = parseSemaAll(wrapWritten(
        "for var entry in e do entry.Value += 1; end; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "read-only positional fields"));
}

// DIFFERENTIAL (v0.334): the two carriers must agree. The written carrier
// rejects (rows above); the SYNTHETIC carrier must STILL reject after the
// widening — `headName()` returns `importedName` verbatim when there is no
// astNode, so a fix that dropped the synthetic carrier would show up here.
BEZEL_TEST(sema_dict_entry_write_624,
           written_spelling_differential_both_carriers_reject_spec_9_4) {
    auto written = parseSemaAll(wrapWritten(
        "for var entry in e do entry.Value := 99; end; return 0;"));
    BEZEL_EXPECT_TRUE(written.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(written, "read-only positional fields"));
    auto synthetic = parseSemaAll(wrap(
        "for var e in d do e.Value := 99; end; return 0;"));
    BEZEL_EXPECT_TRUE(synthetic.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(synthetic, "read-only positional fields"));
}

// §9.4 (v0.334): reads through the written annotation must STILL be accepted.
// This is the reachability guard for the new `headName()` arm — a fix that
// rejected the whole receiver rather than the write would fail here.
BEZEL_TEST(sema_dict_entry_write_624,
           written_spelling_value_read_still_accepted_spec_9_4) {
    auto r = parseSemaAll(wrapWritten(
        "var n := 0; for var entry in e do n := n + entry.Value; end; return n;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §9.4 (v0.334): and so must a read of `entry.Key`.
BEZEL_TEST(sema_dict_entry_write_624,
           written_spelling_key_read_still_accepted_spec_9_4) {
    auto r = parseSemaAll(wrapWritten(
        "var n := 0; for var entry in e do "
        "var k: string := entry.Key; n := n + 1; end; return n;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #2444 / stdlib §4.8 (v0.334): the closed-surface rejection must also fire
// for the written carrier. A member outside DictionaryEntry's documented
// surface is not part of the type; pre-fix this was a SILENT ACCEPT that
// degraded to a placeholder at runtime.
BEZEL_TEST(sema_dict_entry_write_624,
           written_spelling_bogus_member_rejected_spec_4_8) {
    auto r = parseSemaAll(wrapWritten(
        "for var entry in e do var z := entry.TotallyBogusMember; end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(r, "documented surface"));
}

// #2444 DIFFERENTIAL (v0.334): both carriers reject the out-of-surface
// member, and both name the same documented surface.
BEZEL_TEST(sema_dict_entry_write_624,
           written_spelling_bogus_member_differential_spec_4_8) {
    auto written = parseSemaAll(wrapWritten(
        "for var entry in e do var z := entry.TotallyBogusMember; end; "
        "return 0;"));
    BEZEL_EXPECT_TRUE(anyDiagContains(written, "documented surface"));
    auto synthetic = parseSemaAll(wrap(
        "for var e in d do var z := e.TotallyBogusMember; end; return 0;"));
    BEZEL_EXPECT_TRUE(synthetic.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyDiagContains(synthetic, "documented surface"));
}
