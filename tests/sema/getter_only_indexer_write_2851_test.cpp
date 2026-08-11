// Regression tests for #2851 — a write through a GETTER-ONLY user indexer was
// silently discarded, while the named-property twin was correctly rejected.
//
// WHAT THE DOCUMENTS REQUIRE.  §23.4 (v0.240): an indexer is "a property whose
// name is replaced by `[parameters]`" — so §23.3's setter rule governs it
// identically to a named property.  The compiler already enforced that rule for
// the named spelling and not for the indexer one.
//
// MEASURED before the fix, on `bd209a2`:
//     b.P := 99;   -> rejected: "cannot assign to read-only property `P`
//                                — it has no setter (§23.3)"
//     b[0] := 99;  -> compiled clean, ran, and read back 7: the write was
//                     SILENTLY DISCARDED.
//
// THE CAUSE.  The assignment-target check resolved a `PropertySymbol` from a
// `MemberAccessExpr` or an `IdentifierExpr` target only.  An `IndexExpr` target
// left `ps` null, so the entire read-only check was skipped.  The fix adds the
// missing arm, reading the PropertySymbol #2530 attaches to the IndexExpr —
// i.e. asking the type-checker what the access resolved to rather than
// re-deriving it.
//
// WHY THIS CANNOT CATCH BUILTIN COLLECTIONS.  An `Array`/`Dictionary`/span
// index resolves no user `PropertySymbol`, so `ps` stays null and those paths
// are untouched by construction — pinned by accept-controls below rather than
// argued.
//
// ⚠ SCOPE, stated rather than implied.  This covers the `:=` and compound
// (`+=`) spellings, which both route through the assignment-target check.  It
// does NOT cover `b[0]++`, because `IncrementStmt` is a different AST node with
// its own check — and that gap is wider than this issue: `b.P++` on a
// getter-only NAMED property is silently discarded too.  Measured, and filed
// separately; it is not an asymmetry introduced here.
//   ↳ CLOSED by #2868: the assignment-target check moved into
//     `ExprTC::checkPropertyWriteTarget` and `IncrementStmt` now calls it too,
//     so `b[0]++` and `b.P++` are rejected on §17.6's own authority ("Read-only
//     locations cannot be incremented").  Covered by
//     tests/sema/getter_only_increment_2868_test.cpp.  This file's cases are
//     unchanged: the `:=`/`+=` diagnostics are byte-identical after that
//     extraction, which is exactly what these assertions pin.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff an ERROR diagnostic contains every one of `needles`. Asserting on
// diagnostic TEXT rather than the coarse `rejects()` helper is deliberate:
// runAot's WIFSIGNALED branch synthesizes an Error for a runtime SIGSEGV too,
// so `rejects()` alone cannot tell a genuine rejection from a crash.
bool errorSays(const std::string& program,
               std::initializer_list<const char*> needles) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        bool all = true;
        for (const char* n : needles)
            if (d.message.find(n) == std::string::npos) { all = false; break; }
        if (all) return true;
    }
    return false;
}

const char* kGetterOnly =
    "module M; "
    "class B "
    "  private _v: int := 7; "
    "  public property [i: int]: int get => _v + i; end; "
    "  public property P: int get => _v; end P; "
    "end B; ";

const char* kReadWrite =
    "module M; "
    "class RW "
    "  private _v: int := 7; "
    "  public property [i: int]: int "
    "    get => _v + i; "
    "    set => _v := value - i; "
    "  end; "
    "end RW; ";

std::string main_(const char* decls, const std::string& body) {
    return std::string(decls) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

} // namespace

// The reported shape: a plain write through a getter-only indexer. Before the
// fix this compiled and discarded the store.
BEZEL_TEST(getter_only_indexer_write_2851,
           plain_write_through_getter_only_indexer_rejected_spec_23_3) {
    BEZEL_EXPECT_TRUE(errorSays(main_(kGetterOnly,
        "  using var b := new B(); b[0] := 99; return b[0];"),
        {"read-only indexer", "no setter", "23.3"}));
}

// The compound spelling routes through the same check and must be rejected too.
BEZEL_TEST(getter_only_indexer_write_2851,
           compound_write_through_getter_only_indexer_rejected_spec_23_3) {
    BEZEL_EXPECT_TRUE(errorSays(main_(kGetterOnly,
        "  using var b := new B(); b[0] += 1; return b[0];"),
        {"read-only indexer", "no setter"}));
}

// The named twin's diagnostic is unchanged — it names the PROPERTY, not the
// indexer, so the two messages stay distinguishable.
BEZEL_TEST(getter_only_indexer_write_2851,
           named_property_twin_diagnostic_unchanged_spec_23_3) {
    BEZEL_EXPECT_TRUE(errorSays(main_(kGetterOnly,
        "  using var b := new B(); b.P := 99; return 0;"),
        {"read-only property `P`", "no setter"}));
}

// ACCEPT-CONTROL: a read-write indexer must still accept both spellings and
// produce the right values. This is the false-reject guard.
BEZEL_TEST(getter_only_indexer_write_2851,
           read_write_indexer_still_accepted_spec_23_4) {
    auto r = runAot(main_(kReadWrite,
        "  using var b := new RW(); "
        "  b[0] := 99; if b[0] <> 99 then return 91; end; "
        "  b[2] += 1; if b[0] <> 100 then return 92; end; "
        "  return 0;"));
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
}

// ACCEPT-CONTROL: builtin collection indexers resolve no user PropertySymbol,
// so they cannot reach the new arm. Pinned rather than argued.
BEZEL_TEST(getter_only_indexer_write_2851,
           builtin_collection_indexers_untouched_spec_23_4) {
    auto r = runAot(main_(kReadWrite,
        "  var a: Array<int> := [1, 2, 3]; "
        "  a[0] := 42; if a[0] <> 42 then return 93; end; "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"k\"] := 5; if d[\"k\"] <> 5 then return 94; end; "
        "  var s := \"abc\"; if s[1] <> 'b' then return 95; end; "
        "  return 0;"));
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 0);
}

// ACCEPT-CONTROL: a READ through a getter-only indexer is of course fine — the
// rule is about writes only.
BEZEL_TEST(getter_only_indexer_write_2851,
           read_through_getter_only_indexer_accepted_spec_23_4) {
    auto r = runAot(main_(kGetterOnly,
        "  using var b := new B(); return b[3];"));
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 10);
}
