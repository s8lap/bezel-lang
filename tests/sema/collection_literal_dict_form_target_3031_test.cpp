// fix #3031 — a DICTIONARY collection literal `[k: v, ...]` at a sequence-
// shaped target (local decl / return / call argument / lambda body) was
// admitted and silently degraded to a zero-initialized value at codegen,
// the medium-severity silent-wrong-value path that #2434/#2491's
// `internal:` notes paper over. §15.4's last paragraph (v0.240) states
// the form-vs-target split directly: "The dictionary literal form
// `[k1: v1, k2: v2, ...]` (colon-separated pairs) targets only the
// built-in `Dictionary<K, V>` and `OwningDictionary<K, V>`." The
// `isCollectionLiteralAdmissibleTarget` predicate (#1825) used the
// target ALONE, so the form-vs-target mismatch was invisible to sema;
// codegen's two materializers both returned null (`materializeHeapDictLiteral`
// on a non-dict target, `materializeHeapCollectionLiteral` on `cl->isDict`),
// and the local-decl arm degraded to a zero-initialized slot. The
// lambda-body materialiser used the same predicate and silently emitted
// an empty sequence; the call-arg path had only the interface-check
// from #1174 and was equally blind; the return path had no sema gate at
// all (codegen's `lowerReturnStmt` tried both materializers and fell
// through to a zero/null return, #2491). The fix extends the predicate
// to take the literal's `isDict` flag and rejects the form-vs-target
// mismatch at all four positions with a dedicated form-specific
// diagnostic (`reportCollectionLiteralDictFormBadTarget`) and the
// generic "cannot target" message for shape mismatches that pair no
// form against the target — the diagnostics correctly distinguish
// "change the literal's form" from "change the target type".

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True when the compile was rejected AND the rejection is the
// #3031-specific form-vs-target message. The predicate is selective on
// the §15.4 wording and the dictionary-form sentence so the test fails
// if the generic "cannot target" diagnostic replaces it (the wrong
// remediation — "change the target type" — would mislead the user).
bool rejectedByDictFormRule(const JitResult& r, const std::string& spelledTarget) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("dictionary literal `[k: v, ...]` cannot target") !=
                std::string::npos &&
            d.message.find("`" + spelledTarget + "`") != std::string::npos &&
            d.message.find("§15.4") != std::string::npos)
            return true;
    }
    return false;
}

// True when the compile was rejected with the GENERIC §15.4
// "cannot target" diagnostic — used for the inverse case (seq form at
// dict-family target) where the remediation is "change the target type",
// not "change the literal's form".
bool rejectedByGeneralRule(const JitResult& r, const std::string& spelledTarget) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("collection literal `[...]` cannot target") !=
                std::string::npos &&
            d.message.find("`" + spelledTarget + "`") != std::string::npos &&
            d.message.find("§15.4") != std::string::npos)
            return true;
    }
    return false;
}

JitResult decl(const std::string& declText) {
    return runAot("module M; class Main "
                  "public static method Main(): int " + declText +
                  "return 0; end Main; end Main;");
}

}  // namespace

// =====================================================================
// #3031 — REJECT: dict literal at every sequence-shaped §15.4 target,
// at each of the four literal positions. Pre-fix the local-decl arm
// degraded to a zero-initialized value (the #2434 `#internal:` line),
// the call-arg arm silently swallowed the literal, the return arm
// degraded to a zero/null return (#2491), and the lambda body emitted
// an empty sequence. Every cell is now a loud compile error.
// =====================================================================

BEZEL_TEST(sema_collection_literal_dict_form_target_3031,
           local_decl_array_int_rejected_spec_15_4) {
    BEZEL_EXPECT_TRUE(
        rejectedByDictFormRule(decl("var a: Array<int> := [1: 2, 3: 4];"),
                               "Array<int>"));
}

BEZEL_TEST(sema_collection_literal_dict_form_target_3031,
           local_decl_stack_int_rejected_spec_15_4) {
    BEZEL_EXPECT_TRUE(
        rejectedByDictFormRule(decl("using var s: Stack<int> := [1: 2];"),
                               "Stack<int>"));
}

BEZEL_TEST(sema_collection_literal_dict_form_target_3031,
           local_decl_set_int_rejected_spec_15_4) {
    BEZEL_EXPECT_TRUE(
        rejectedByDictFormRule(decl("using var s: Set<int> := [1: 2];"),
                               "Set<int>"));
}

BEZEL_TEST(sema_collection_literal_dict_form_target_3031,
           local_decl_owning_array_int_rejected_spec_15_4) {
    BEZEL_EXPECT_TRUE(rejectedByDictFormRule(
        decl("using var oa: OwningArray<int> := [1: 2, 3: 4];"),
        "OwningArray<int>"));
}

BEZEL_TEST(sema_collection_literal_dict_form_target_3031,
           call_arg_array_int_rejected_spec_15_4) {
    JitResult r = runAot(
        "module M; class Main "
        "public static method Take(xs: Array<int>): int return xs.Count; end Take; "
        "public static method Main(): int "
        "return Take([1: 2, 3: 4]); "
        "end Main; end Main;");
    BEZEL_EXPECT_TRUE(rejectedByDictFormRule(r, "Array<int>"));
}

BEZEL_TEST(sema_collection_literal_dict_form_target_3031,
           return_array_int_rejected_spec_15_4) {
    JitResult r = runAot(
        "module M; class Main "
        "public static method Mk(): Array<int> return [1: 2, 3: 4]; end Mk; "
        "public static method Main(): int return Mk().Count; end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(rejectedByDictFormRule(r, "Array<int>"));
}

BEZEL_TEST(sema_collection_literal_dict_form_target_3031,
           lambda_body_array_int_rejected_spec_15_4) {
    JitResult r = runAot(
        "module M; class Main "
        "public static method Main(): int "
        "var f: (int) -> Array<int> := lambda(x) => [1: x, 2: x]; "
        "return f(0); "
        "end Main; end Main;");
    BEZEL_EXPECT_TRUE(rejectedByDictFormRule(r, "Array<int>"));
}

// =====================================================================
// #3031 — REJECT: the inverse (seq form at dict target) keeps the
// generic "cannot target" diagnostic — the remediation is to change the
// target type, not the literal's form. The two form-specific and
// generic messages are deliberately distinct so the user is told the
// right thing.
// =====================================================================

BEZEL_TEST(sema_collection_literal_dict_form_target_3031,
           seq_at_dict_target_keeps_generic_message_spec_15_4) {
    BEZEL_EXPECT_TRUE(rejectedByGeneralRule(
        decl("var d: Dictionary<string, int> := [1, 2, 3];"),
        "Dictionary<string, int>"));
}

// =====================================================================
// #3031 — ACCEPT-CONTROLS: §15.4's literal/target pairing is still
// enforced, not over-narrowed. The dict form at a dict-family target
// still materializes; the seq form at every sequence-shaped target
// still materializes; the empty literal at a dict target is admitted
// (the spec lets the target type decide between empty seq and empty
// dict — `Dictionary<string,int> := []` is a valid empty dict). The
// expected value is deliberately a non-zero number so a vacuous
// "compiled and ran" pass cannot certify a degraded value.
// =====================================================================

BEZEL_TEST(sema_collection_literal_dict_form_target_3031,
           dict_form_at_dict_target_still_materializes_spec_15_4) {
    JitResult r = runAot("module M; "
                        "class Main "
                        "public static method Main(): int "
                        "var d: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
                        "return d.Count; "
                        "end Main; end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

BEZEL_TEST(sema_collection_literal_dict_form_target_3031,
           seq_form_at_seq_target_still_materializes_spec_15_4) {
    JitResult r = runAot("module M; "
                        "class Main "
                        "public static method Main(): int "
                        "var a: Array<int> := [1, 2, 3]; "
                        "return a.Count; "
                        "end Main; end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

BEZEL_TEST(sema_collection_literal_dict_form_target_3031,
           empty_literal_at_dict_target_still_accepted_spec_15_4) {
    // §15.4 "the empty literal `[]` is admitted for every collection
    // target listed above … the target type determines whether `[]`
    // constructs an empty sequence collection or an empty dictionary"
    // — the empty literal at a dict target is an empty dict (Count=0),
    // NOT a degraded zero-initialized slot.
    JitResult r = runAot("module M; "
                        "class Main "
                        "public static method Main(): int "
                        "var d: Dictionary<string, int> := []; "
                        "return d.Count; "
                        "end Main; end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(sema_collection_literal_dict_form_target_3031,
           empty_literal_at_seq_target_still_accepted_spec_15_4) {
    // The empty literal at a sequence target is the empty-sequence
    // form (the count-zero shape).
    JitResult r = runAot("module M; "
                        "class Main "
                        "public static method Main(): int "
                        "var a: Array<int> := []; "
                        "return a.Count; "
                        "end Main; end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
