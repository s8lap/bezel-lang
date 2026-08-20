// Sema (via the AOT compile path) — issue #2499: an enum BLOCK-FORM property
// (`get`/`set` accessor-block syntax) and an enum AUTO-PROPERTY (backing
// storage) were both silently ACCEPTED, contra language spec §10.5
// (Restrictions):
//
//   "An enum may not declare: ... Block-form properties or properties with
//   backing storage. ... Indexers."
//
// The indexer half of that sentence was already enforced
// (`Resolver::checkEnumRestrictions`, src/sema/TypeResolution_Checks.cpp);
// the block-form/backing-storage half had no check at all — the
// `PropertySymbol` branch there only ever tested `ps->isIndexer`.
//
// §10.4 (Methods, properties, static members) is the companion permission:
//
//   "An enum may declare instance methods, expression-bodied read-only
//   properties, static methods, static read-only fields..., `const`
//   members, and operator overloads."
//
// Grammar A.4 confirms the expression-bodied form is inherently read-only
// (`ExpressionPropertyDecl ::= ... "property" Identifier ":" Type "=>"
// Expression ";"` — the "=>" production has no setter alternative), so
// `PropertyBodyKind::Expression` is always the §10.4-legal shape and needs
// no separate settability check; `PropertyBodyKind::Block` (get/set
// accessor-block syntax) and `PropertyBodyKind::Auto` (backing storage) are
// both forbidden regardless of whether they declare a setter — a
// getter-only block-form property is still "block-form".
//
// Fix: `Resolver::checkEnumRestrictions`'s PropertySymbol branch now also
// inspects the underlying `ast::PropertyDecl::bodyKind` and rejects Block
// and Auto, leaving Expression (and the pre-existing indexer check)
// untouched.

#include "support/aot_support.hpp"   // runAot — AOT compile path
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must
// be distinguished by asserting on the diagnostic TEXT, never by exit code
// or `!r.ran` alone.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ── §10.5: block-form properties are rejected, settable or not ──────────

BEZEL_TEST(enum_property_restrictions_2499,
           block_form_settable_property_rejected_spec_10_5) {
    const std::string program =
        "module M; "
        "public enum E "
        "  Red, Green; "
        "  public property P: int "
        "    get => 7; "
        "    set => do end; "
        "  end P; "
        "end E; "
        "public class C public static method Main(): int return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "may not declare a block-form property"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""10.5"));
}

// A GETTER-ONLY block-form property is still "block-form" per §10.5's
// unqualified wording — not merely the settable case.
BEZEL_TEST(enum_property_restrictions_2499,
           block_form_getter_only_property_rejected_spec_10_5) {
    const std::string program =
        "module M; "
        "public enum E "
        "  Red, Green; "
        "  public property P: int "
        "    get => 7; "
        "  end P; "
        "end E; "
        "public class C public static method Main(): int return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "may not declare a block-form property"));
}

// ── §10.5: a property with backing storage (auto-property) is rejected ──

BEZEL_TEST(enum_property_restrictions_2499,
           auto_property_backing_storage_rejected_spec_10_5) {
    const std::string program =
        "module M; "
        "public enum E "
        "  Red, Green; "
        "  public property P: int := 5; "
        "end E; "
        "public class C public static method Main(): int return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "may not declare a property with backing storage"));
    BEZEL_EXPECT_TRUE(hasError(program, "\xC2\xA7""10.5"));
}

// ── §10.4 regression guard: the expression-bodied read-only form MUST
//    still compile AND run correctly — the false-reject risk this fix
//    must avoid. This is the exact worked example from §10.4, respelled
//    to the v0.256 FrozenArray<Priority> form. ──────────────────────────

BEZEL_TEST(enum_property_restrictions_2499,
           expression_bodied_readonly_property_still_compiles_spec_10_4) {
    const std::string program =
        "module M; "
        "enum Priority "
        "  Low, Medium, High; "
        "  public method IsUrgent(): bool => this = High; "
        "  public property Name: string => (case this of "
        "    Low => \"Low\", "
        "    Medium => \"Medium\", "
        "    High => \"High\"); "
        "  public static readonly All: FrozenArray<Priority> := "
        "      FrozenArray<Priority>.From([Low, Medium, High]); "
        "  public static method Parse(text: string): Priority? "
        "    return (case text of "
        "      \"Low\" => Low, "
        "      \"Medium\" => Medium, "
        "      \"High\" => High, "
        "      _ => null); "
        "  end Parse; "
        "end Priority; "
        "public class C "
        "  public static method Main(): int "
        "    var ok := Priority.High.IsUrgent() "
        "      and Priority.Low.Name = \"Low\" "
        "      and Priority.All.Length = 3 "
        "      and Priority.Parse(\"Medium\") = Priority.Medium; "
        "    return (if ok then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Regression guard: the pre-existing indexer rejection (same function,
//    same PropertySymbol branch) is unaffected by the new bodyKind check. ─

BEZEL_TEST(enum_property_restrictions_2499,
           indexer_still_rejected_spec_10_5) {
    const std::string program =
        "module M; "
        "public enum E "
        "  Red, Green; "
        "  public property [i: int]: int => 7; "
        "end E; "
        "public class C public static method Main(): int return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(program, "may not declare an indexer"));
}
