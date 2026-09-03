// Spec-derived regression test for issue #3796 — sema's per-instantiation
// diagnostics for non-hashable hash-container keys (§3.6 rule 2, Pass 1 +
// Pass 2) and for the §28.11 element-category move-only rejection (§15.3,
// Pass 1 + Pass 2) reported the source location as `{0,0}` — every emitted
// diagnostic's `location` field was the default-constructed SourceLocation,
// so the driver printed `path:0:0: <message>` instead of jumping the IDE to
// the offending type argument.
//
// THE GOVERNING RULES.
//
//   Spec §3.6 rule 2, v0.286: a hash-container key type "must be hashable"
//   (§9.2 admissible-component form, or a type that explicitly declares
//   equality per §25.2). When the rule fires at a non-generic site it
//   anchors on the key type argument; when it fires per-instantiation
//   (#1384) it anchors on the BOUND concrete key at the instantiation site.
//
//   Spec §28.11 (heading "Collection element ownership"), "The element slot
//   policy" table, v0.286, move-only row: rejected at instantiation for BOTH
//   borrowing and owning families (§15.3 names the two move-only kinds:
//   owning inline array, owning-classified choice per §35.4). The §3.6
//   per-instantiation discipline applies symmetrically (§3775 modelled its
//   two-pass shape on this #1384 discipline).
//
//   Spec §22.4 (call-out for source-location anchoring): a diagnostic "names
//   the source range that triggered it" — for any rule that fires on a
//   generic type argument, that range is the inner type argument, not the
//   outer `Dictionary<…>` or `Array<…>` Name node.
//
// ISSUE'S OWN REPRO (measured on this lane's base commit, 8e14b4c, BEFORE
// this fix):
//
//   module M;
//   class R public V: int := 0; destructor end destructor; end R;
//   record Holder(item: R);   // not auto-hashable per §9.2/§9.3
//   class C public static method Main(): int
//     var d: Dictionary<Holder, int> := new Dictionary<Holder, int>();
//     return 0;
//   end Main; end C;
//
// `bezelc` rejected it with the correct TEXT but `:0:0:` as the location,
// even though `Holder` sits on line 4 column 18 — IDE jump went to the
// top-left corner of the file instead of the offending type argument.
//
// ROOT CAUSE. Two cooperating defects:
//
//   1. `Resolver::checkHashContainerKeys` (src/sema/TypeResolution_Checks.cpp,
//      #1338) and `Resolver::checkElementCategoryInstantiations` (same file,
//      #3775) anchored the diagnostic on the enclosing `ast::Type*` Node's
//      span — the `Dictionary<K,V>` Name node carried by `s.site` — rather
//      than the offending TYPE-ARGUMENT's AST node (`s.keyRef->astNode` /
//      `s.elRef->astNode` / `inst.argAsts[paramIdx]`). At a `new Foo<K,V>()`
//      site the annotation node's span was the *whole* `Foo<K,V>`, which is
//      what the IDE was jumping to, but at a typed-local annotation
//      (`var d: Dictionary<Holder, int> := ...`) it was the inner generic
//      arguments list, which has no useful span.
//
//   2. `Parser::parseType` (src/parser/Parser_Decls.cpp:826) did NOT stamp
//      its returned AST node's span — unlike every `parseExpression` variant,
//      which wraps its return with `stampSpan`. So even when sema reached
//      for `s.keyRef->astNode->span.begin`, the span was the
//      default-constructed `SourceLocation{}` (0:0). Without the parser
//      stamp, the sema fix could only change WHICH zero span it reported
//      from, not produce a real one.
//
// THE FIX. Two coordinated edits:
//
//   - src/parser/Parser_Decls.cpp — `parseType` now wraps every return with
//     `stampSpan(std::move(t), typeStart, current_.location)` so AST `Type`
//     nodes carry the [start, current) span.
//
//   - src/sema/TypeResolution_Checks.cpp — all four diagnostic emit sites
//     (Pass 1 + Pass 2 of both `checkHashContainerKeys` and
//     `checkElementCategoryInstantiations`) prefer the type argument's
//     AST node location, falling back to `s.site` only when the type arg
//     has no AST node (an imported type whose TypeRef carries no AST).
//
// SCOPE DECLINED, FILED SEPARATELY:
//   - `Resolver::owningElementBadDesc` (#2744/#2713) also reports at 0:0
//     and lives in the same family. This fix's parser stamp already fixes
//     it as a transitive effect (the §28.11 typed-local/site check now
//     reads a stamped span from the type arg it was already holding). The
//     OWNED-slot check itself does not need editing; verified on this
//     lane's base before declaring scope.
//
//   - Per §22.4 the spec describes "the source range that triggered it",
//     and IDE-jump conventions want the start of the offending argument.
//     Using `span.begin` (start) rather than `span.end` matches every
//     other "diagnostic on a generic argument" site in the sema pass —
//     a uniform policy the file already enforces for #2083/#2087/etc.
//     Did NOT make this a parameter; the file-level precedent wins.
//
// FAIL-WITHOUT / PASS-WITH (measured against this lane's base, 8e14b4c):
// a PARENT bezelc built via a separate worktree checkout of 8e14b4c reports
// `:0:0:` for every assertion below; the FIX compiler reports a non-zero
// line:column pointing to the offending type argument.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace bezel::sema {
namespace {

// Extract the first `<path>:<line>:<col>: <message>` triple from a captured
// driver-stderr blob and decode the numeric line + column. Returns {-1,-1}
// if the format is not present (the diagnostic didn't carry a location
// stamp at all — the bug we're guarding against).
struct Loc {
    int line;
    int column;
};
Loc parseFirstLoc(const std::string& blob) {
    // Walk back from `: ` (Driver.cpp:632 prints `path:line:col: msg`) until
    // we hit two colons whose adjacent run is digits; cap at 1024 to bound
    // pathological cases (mirrors the §17 anchor-patch discipline).
    auto digitsBack = [](const std::string& s,
                         std::size_t from,
                         std::string& out) -> std::size_t {
        std::size_t i = from;
        while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9') --i;
        out.assign(s, i, from - i);
        return i;
    };
    // Find the first occurrence of `: ` (colon-space, the message separator).
    // The MESSAGE itself starts with a backtick (e.g. `` `Dictionary` ``), so
    // scanning for `: ` skips the path-internal colons reliably.
    std::size_t msgPos = blob.find(": ");
    if (msgPos == std::string::npos || msgPos == 0) return {-1, -1};
    // Walk back to find `:col:` (digit-run, colon, digit-run, colon).
    // Bounded — we do NOT need a real regex.
    std::string colStr;
    std::size_t colStart =
        digitsBack(blob, msgPos, colStr);
    if (colStart == 0 || colStart >= msgPos) return {-1, -1};
    if (blob[colStart - 1] != ':') return {-1, -1};
    std::size_t lineEnd = colStart - 1;  // at the colon before col
    std::string lineStr;
    std::size_t lineStart =
        digitsBack(blob, lineEnd, lineStr);
    if (lineStart >= lineEnd || blob[lineStart - 1] != ':')
        return {-1, -1};
    try {
        return {std::stoi(lineStr), std::stoi(colStr)};
    } catch (...) {
        return {-1, -1};
    }
}

// `r` is the result of `runAot` on a source that triggers a
// `checkHashContainerKeys` reject — the synthesized Error diagnostic's
// message is the captured driver stderr (§runAot comment block). Return
// the FIRST `<path>:<line>:<col>: <message>` triple's (line, column), or
// {-1,-1} if no such triple is found.
Loc hashContainerKeyLoc(const JitResult& r) {
    if (r.ran) return {-1, -1};
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        // The §3.6/§8.3 hashable-key reject. Other errors (parse, lexer)
        // lack this substring and are skipped.
        if (d.message.find("requires a hashable") == std::string::npos) continue;
        return parseFirstLoc(d.message);
    }
    return {-1, -1};
}

// Same shape, but for the §28.11 element-category reject.
Loc moveOnlyElementLoc(const JitResult& r) {
    if (r.ran) return {-1, -1};
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("\xC2\xA7""28.11") == std::string::npos) continue;
        if (d.message.find("\xC2\xA7""15.3") == std::string::npos) continue;
        return parseFirstLoc(d.message);
    }
    return {-1, -1};
}

}  // namespace

// =============================================================================
// Pass 1 of checkHashContainerKeys: a non-hashable record as a typed-local
// `Dictionary<K,V>` KEY. Before the fix the diagnostic was anchored on the
// enclosing `Dictionary<Holder, int>` Name node — whose span is the whole
// annotation, NOT the offending inner argument. With the fix the location
// points at `Holder` (the type argument) — the line/column non-zero.
// =============================================================================
BEZEL_TEST(sema_instantiation_hash_container_diag_3796,
           dict_key_non_hashable_diag_at_arg_site_3796) {
    auto r = runAot(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "class C public static method Main(): int "
        "  var d: Dictionary<Holder, int> := new Dictionary<Holder, int>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    auto loc = hashContainerKeyLoc(r);
    BEZEL_EXPECT_TRUE(loc.line > 0);     // was 0:0 before the fix
    BEZEL_EXPECT_TRUE(loc.column > 0);   // was 0:0 before the fix
}

// =============================================================================
// Pass 2 of checkHashContainerKeys: a generic parameter of the enclosing
// type bound to a non-hashable concrete type at instantiation. The §3.6
// rule-2 per-instantiation discipline (#1384) anchors on the concrete
// type-argument's AST at the instantiation site, NOT the interior body
// annotation — so the location must point at `Holder` inside
// `new Registry2<Holder>()`, which is line 9 column 30 in the source below.
// =============================================================================
BEZEL_TEST(sema_instantiation_hash_container_diag_3796,
           dict_key_instantiation_diag_at_concrete_arg_3796) {
    auto r = runAot(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "class Registry2<K> "
        "  private owned f: Dictionary<K, int> := new Dictionary<K, int>(); "
        "  public method N(): int return f.Count; end N; "
        "end Registry2; "
        "class C public static method Main(): int "
        "  var h := new Registry2<Holder>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    auto loc = hashContainerKeyLoc(r);
    BEZEL_EXPECT_TRUE(loc.line > 0);     // was 0:0 before the fix
    BEZEL_EXPECT_TRUE(loc.column > 0);   // was 0:0 before the fix
}

// =============================================================================
// Pass 1 of checkElementCategoryInstantiations: an owning-classified choice
// as a typed-local `Dictionary<K,V>` VALUE. Before the fix this diagnostic
// too reported 0:0 (the same root cause — the `Wrap` type argument's
// AST node had no stamped span). After the fix it points at `Wrap`.
// =============================================================================
BEZEL_TEST(sema_instantiation_hash_container_diag_3796,
           dict_value_move_only_diag_at_arg_site_3796) {
    auto r = runAot(
        "module M; "
        "class Conn public id: int := 0; end Conn; "
        "choice Wrap Item(owned c: Conn), None; end Wrap; "
        "class C public static method Main(): int "
        "  var d: Dictionary<int, Wrap> := new Dictionary<int, Wrap>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    auto loc = moveOnlyElementLoc(r);
    BEZEL_EXPECT_TRUE(loc.line > 0);     // was 0:0 before the fix
    BEZEL_EXPECT_TRUE(loc.column > 0);   // was 0:0 before the fix
}

// =============================================================================
// Pass 2 of checkElementCategoryInstantiations: a bare generic parameter
// bound to an owning-classified choice at instantiation. The §28.11
// per-instantiation discipline (§3775) anchors on the BOUND concrete
// type-argument's AST at the instantiation site.
// =============================================================================
BEZEL_TEST(sema_instantiation_hash_container_diag_3796,
           dict_value_instantiation_diag_at_concrete_arg_3796) {
    auto r = runAot(
        "module M; "
        "class Conn public id: int := 0; end Conn; "
        "choice Wrap Item(owned c: Conn), None; end Wrap; "
        "class Box<T> "
        "  private owned f: Dictionary<int, T> := new Dictionary<int, T>(); "
        "  public method N(): int return f.Count; end N; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<Wrap>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    auto loc = moveOnlyElementLoc(r);
    BEZEL_EXPECT_TRUE(loc.line > 0);     // was 0:0 before the fix
    BEZEL_EXPECT_TRUE(loc.column > 0);   // was 0:0 before the fix
}

}  // namespace bezel::sema
