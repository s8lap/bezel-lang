// fix #3831 — impl-ahead removal: `SortedDictionary<K, V>` was wrongly
// admitted as a collection-literal target (spec §15.4, ruled on #3517).
//
// WHAT THE SPEC REQUIRES. §15.4's target-set paragraph (v0.294, unchanged
// since v0.240) is a CLOSED list, and its last paragraph states the
// dictionary form's target set as exactly two names:
//
//   "The dictionary literal form `[k1: v1, k2: v2, ...]` (colon-separated
//    pairs) targets only the built-in `Dictionary<K, V>` and
//    `OwningDictionary<K, V>`."
//
// `SortedDictionary<K, V>` is not one of the two names, and it is not a
// §15.4.1 sequence-literal-protocol conformer either (its `Contains`/
// `ContainsKey` surface is the .NET-familiarity alias of stdlib §1's
// "Key membership is `Contains(key)`" conventions row,
// unrelated to `ICollectionLiteralTarget`/`IOwningCollectionLiteralTarget`),
// so no literal FORM targets it at all. The ruling on #3517 confirms the
// closed list stands and that widening it is an undecided proposal question
// (#3489), not current behavior.
//
// THE PRE-FIX BUG. `isCollectionLiteralAdmissibleTarget` and
// `isCollectionLiteralEmptyAdmissibleTarget` (both in
// ExprTypecheck_Calls.cpp) matched `"SortedDictionary"` alongside
// `"Dictionary"`/`"OwningDictionary"`, so `var sd: SortedDictionary<int,
// string> := [1: "a"];` compiled. Codegen's `materializeHeapDictLiteral`
// (LlvmBackend_Setup.cpp) then retargeted the literal's hash/eq trampolines
// onto `SortedDictionary`'s `CompareTo`-identity — the path that leaked the
// backing table (#3517 item 1).
//
// THE FIX removes `"SortedDictionary"` from both admission predicates (both
// the dict-form and the empty-form checks), so every literal form (dict,
// sequence, and empty) is rejected at a `SortedDictionary<K, V>` target, at
// every position §15.4 governs. `reportCollectionLiteralBadTarget` now
// special-cases the `SortedDictionary` head to name the concrete remedy
// (`new SortedDictionary<K, V>()` + `.Add(k, v)`) rather than the generic
// "pick a listed target" nudge. `new SortedDictionary<K, V>()` construction
// itself, and every instance method on it, are completely unaffected — only
// the literal-construction admission is removed.

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True when the compile was rejected with a diagnostic naming BOTH the
// `SortedDictionary<...>` target and §15.4 — selective enough that a
// generic "wrong number of args" or unrelated parse error cannot pass it.
bool rejectedNamingSortedDictionary(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SortedDictionary") != std::string::npos &&
            d.message.find("§15.4") != std::string::npos)
            return true;
    }
    return false;
}

// True when the rejection is the #3831-specific message naming the
// concrete remedy (`new SortedDictionary<K, V>()` + `.Add`), not just any
// message that happens to mention the type.
bool rejectedWithRemedy(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("cannot target the type") != std::string::npos &&
            d.message.find("SortedDictionary") != std::string::npos &&
            d.message.find("new SortedDictionary") != std::string::npos &&
            d.message.find(".Add(k, v)") != std::string::npos)
            return true;
    }
    return false;
}

// Per LANE_RULES §3: `runAot`'s diagnostics for a REJECT carry no real
// per-diagnostic SourceLocation — the harness packs captured stderr into
// one synthetic Diagnostic at {0,0}. To confirm the rejection is anchored
// at a REAL source location (not `0:0`), parse the `path:line:col: ` prefix
// out of the message text itself, on the line naming the target.
bool hasErrorWithRealLocation(const JitResult& r, const char* sub) {
    static const std::regex kLoc(R"(:(\d+):(\d+): )");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::size_t pos = d.message.find(sub);
        if (pos == std::string::npos) continue;
        std::size_t lineStart = d.message.rfind('\n', pos);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        std::size_t lineEnd = d.message.find('\n', pos);
        std::string line = d.message.substr(
            lineStart, (lineEnd == std::string::npos ? d.message.size()
                                                      : lineEnd) -
                           lineStart);
        std::smatch m;
        if (!std::regex_search(line, m, kLoc)) continue;
        int lineNo = std::stoi(m[1].str());
        int colNo = std::stoi(m[2].str());
        if (lineNo != 0 && colNo != 0) return true;
    }
    return false;
}

std::string prog(const std::string& body) {
    return "module M; class Main "
           "public static method Main(): int " + body +
           "end Main; end Main;";
}

}  // namespace

// =====================================================================
// REJECT — dictionary-literal form `[k: v, ...]` at a SortedDictionary
// local-decl target.
// =====================================================================
BEZEL_TEST(sorted_dictionary_literal_target_removed_3831,
           local_decl_dict_form_rejected_spec_15_4) {
    JitResult r = runAot(prog(
        "var sd: SortedDictionary<int, string> := [1: \"a\", 2: \"b\"]; "
        "return sd.Count; "));
    BEZEL_EXPECT_TRUE(rejectedNamingSortedDictionary(r));
    BEZEL_EXPECT_TRUE(rejectedWithRemedy(r));
    BEZEL_EXPECT_TRUE(
        hasErrorWithRealLocation(r, "cannot target the type"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// =====================================================================
// REJECT — sequence-literal form `[e1, e2, ...]` at a SortedDictionary
// target. SortedDictionary has no sequence-literal form either (it does
// not conform to §15.4.1's ICollectionLiteralTarget), so this is rejected
// exactly as the dict form is.
// =====================================================================
BEZEL_TEST(sorted_dictionary_literal_target_removed_3831,
           local_decl_seq_form_rejected_spec_15_4) {
    JitResult r = runAot(prog(
        "var sd: SortedDictionary<int, string> := [1, 2]; "
        "return sd.Count; "));
    BEZEL_EXPECT_TRUE(rejectedNamingSortedDictionary(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// =====================================================================
// REJECT — the empty literal `[]` at a SortedDictionary target. §15.4's
// empty-literal carve-out admits `[]` at every target ON the closed list;
// SortedDictionary is off the list, so `[]` is rejected too, not silently
// accepted as an empty map.
// =====================================================================
BEZEL_TEST(sorted_dictionary_literal_target_removed_3831,
           local_decl_empty_literal_rejected_spec_15_4) {
    JitResult r = runAot(prog(
        "var sd: SortedDictionary<int, string> := []; "
        "return sd.Count; "));
    BEZEL_EXPECT_TRUE(rejectedNamingSortedDictionary(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// =====================================================================
// REJECT — call-argument position: a dict literal passed directly to a
// `SortedDictionary<K, V>`-typed parameter.
// =====================================================================
BEZEL_TEST(sorted_dictionary_literal_target_removed_3831,
           call_arg_dict_form_rejected_spec_15_4) {
    JitResult r = runAot(
        "module M; class Main "
        "public static method Take(sd: SortedDictionary<int, string>): int "
        "  return sd.Count; "
        "end Take; "
        "public static method Main(): int "
        "  return Take([1: \"a\"]); "
        "end Main; end Main;");
    BEZEL_EXPECT_TRUE(rejectedNamingSortedDictionary(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// =====================================================================
// REJECT — return position: a dict literal returned as
// `SortedDictionary<K, V>`.
// =====================================================================
BEZEL_TEST(sorted_dictionary_literal_target_removed_3831,
           return_dict_form_rejected_spec_15_4) {
    JitResult r = runAot(
        "module M; class Main "
        "public static method Mk(): SortedDictionary<int, string> "
        "  return [1: \"a\"]; "
        "end Mk; "
        "public static method Main(): int "
        "  return Mk().Count; "
        "end Main; end Main;");
    BEZEL_EXPECT_TRUE(rejectedNamingSortedDictionary(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// =====================================================================
// REJECT — field-initializer position: `public D: SortedDictionary<K,V>
// := [...]`.
// =====================================================================
BEZEL_TEST(sorted_dictionary_literal_target_removed_3831,
           field_initializer_dict_form_rejected_spec_15_4) {
    JitResult r = runAot(
        "module M; class Holder "
        "  public D: SortedDictionary<int, string> := [1: \"a\"]; "
        "end Holder; "
        "class Main "
        "  public static method Main(): int return 0; end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(rejectedNamingSortedDictionary(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// =====================================================================
// ACCEPT-CONTROL — `new SortedDictionary<K, V>()` + `.Add` construction
// (the named remedy) is completely unaffected: this is the actual
// construction path, distinct from the removed literal admission.
// =====================================================================
BEZEL_TEST(sorted_dictionary_literal_target_removed_3831,
           new_plus_add_construction_still_works_stdlib_4_17) {
    JitResult r = runAot(prog(
        "using var sd := new SortedDictionary<int, string>(); "
        "sd.Add(1, \"a\"); "
        "sd.Add(2, \"b\"); "
        "return sd.Count; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// =====================================================================
// ACCEPT-CONTROL — the dict-literal form at `Dictionary<K, V>` (one of
// the two names §15.4 DOES admit) still materializes; guards against an
// over-broad fix that accidentally narrowed the two-name list itself.
// =====================================================================
BEZEL_TEST(sorted_dictionary_literal_target_removed_3831,
           dictionary_literal_target_unaffected_spec_15_4) {
    JitResult r = runAot(prog(
        "using var d: Dictionary<int, string> := [1: \"a\", 2: \"b\"]; "
        "return d.Count; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// =====================================================================
// ACCEPT-CONTROL — the dict-literal form at `OwningDictionary<K, V>` (the
// other of the two names) still materializes. `OwningDictionary<K, V>`
// requires a class-typed `V` (stdlib §4.2/§14.4 — a value type has no
// disposal obligation and cannot be owned), and §15.4's borrowing-family
// element rule requires each such value to be a fresh producer (`new
// Conn(...)`), so the value cannot be `string` here (unlike the
// `Dictionary<K, V>` control above).
// =====================================================================
BEZEL_TEST(sorted_dictionary_literal_target_removed_3831,
           owning_dictionary_literal_target_unaffected_spec_15_4) {
    JitResult r = runAot(
        "module M; "
        "class Conn(id: int) "
        "  public Id: int := id; "
        "  destructor end destructor; "
        "end Conn; "
        "class Main "
        "public static method Main(): int "
        "using var d: OwningDictionary<int, Conn> := "
        "    [1: new Conn(10), 2: new Conn(20), 3: new Conn(30)]; "
        "return d.Count; "
        "end Main; end Main;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}
