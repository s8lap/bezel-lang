// fix #8350 / #8351 (stdlib §4.6-§4.10, §4.17-§4.19; language spec §26.5 /
// §27.6, v0.355): the BARE-REFERENCE path of the built-in collection surface.
//
// TWO DEFECTS, ONE TABLE.
//
// #8350 — THE §4.19 HEAD HAD NO BARE-REFERENCE ROW. `fix #8331` made sema's
// CALL-surface admission table-driven: the authority is the standard library's
// own per-head surface table (`concreteCollectionSurfaces()` in
// `src/sema/ExprTypecheck_Calls.cpp`), and every head in it draws the stdlib's
// documented-surface diagnostic when a call names an undeclared member. The
// BARE form (`var x := coll.Member;` — a method reference, language spec
// §27.6) goes through a DIFFERENT path, and that path had no row for
// stdlib §4.19's `FrozenArray<T>` at all: a bare reference to a member §4.19
// does not declare compiled clean and silently degraded to a zero-initialized
// slot (#2434/#2547), while the CALL form of the very same name was refused.
//
// The fix gives the bare-reference path the SAME table row the CALL path
// reads, in BOTH shapes a built-in collection type carries:
//   * the ANNOTATION shape — a declaration-less Named TypeRef with an AST node
//     (`var f: FrozenArray<int> := ...`); and
//   * the FACTORY-RESULT (builtin-class STUB) shape — `FrozenArray<int>.From(a)`
//     stamps the synthetic `builtinClassSym("FrozenArray")` on its result
//     (`var f := FrozenArray<int>.From(a);`).
// The stub shape could not reach the old catch-all at all: its receiver
// carries a TypeSymbol, so `resolveMemberAccess` never enters the
// `!targetType` block the catch-all lives in — which is exactly why the two
// shapes of ONE type disagreed about ONE head's surface.
//
// #8351 — THE BARE-REFERENCE DIAGNOSTIC QUOTED A HAND-WRITTEN LIST. The
// hand-written per-family lists omitted members the standard library
// DECLARES: `SortedDictionary<K,V>`'s omitted `Add`, `Count` and `TryAdd`
// (stdlib §4.17), `Dictionary<K,V>`'s omitted `Count`, `Keys`, `Values` and
// `TryAdd` (stdlib §4.8), `OwningDictionary<K,V>`'s omitted `Count`, `Keys`
// and `Values` (stdlib §4.9). A diagnostic that confidently quotes a surface
// the document does not have is worse than a generic "no member". The quoted
// list is now GENERATED from the very row the decision was made against, so
// the list and the gate can never disagree.
//
// ORACLE: `runAot` (the AOT pipeline). The reject rows assert the SPECIFIC
// diagnostic TEXT (never the bare presence of an error, so a reject for an
// unrelated reason cannot pass); the admit rows assert the program's stdout,
// so a row cannot pass by accident.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The stdlib's documented-surface diagnostic: "no member `M` on `Head`
// (stdlib §4.x) -- it is not part of this collection's documented surface
// (…), a universal `ToString`/… member (language spec §8.1), or a stdlib §5
// LINQ extension operator". Asserted by TEXT.
bool hasNoMemberDiagnostic(const JitResult& r, const char* member,
                           const char* head) {
    const std::string needle =
        std::string("no member `") + member + "` on `" + head + "`";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The stdlib SECTION the diagnostic cites ("stdlib §4.19").
bool citesStdlibSection(const JitResult& r, const char* section) {
    const std::string needle = std::string("stdlib \xC2\xA7") + section;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The QUOTED surface list — the parenthesised member list the diagnostic
// carries. Returns "" when the diagnostic does not carry one.
std::string quotedSurface(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        std::size_t open = d.message.find("documented surface (");
        if (open == std::string::npos) continue;
        open += std::string("documented surface (").size();
        std::size_t close = d.message.find(')', open);
        if (close == std::string::npos) continue;
        return d.message.substr(open, close - open);
    }
    return "";
}

// True when the quoted list carries `member` as one of its "/"-joined names.
// The list is generated from the head's own table row, so it is a "/"-joined
// list of member names — split it and compare element by element.
bool quotedSurfaceHas(const std::string& surface, const char* member) {
    std::size_t start = 0;
    while (start <= surface.size()) {
        std::size_t slash = surface.find('/', start);
        std::string name = surface.substr(
            start, slash == std::string::npos ? std::string::npos
                                              : slash - start);
        if (name == member) return true;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return false;
}

// The program's captured stdout with ONE trailing line-ending stripped — the
// same normalisation `runAotOut` performs, so the expectation is
// platform-independent.
std::string printed(const JitResult& r) {
    std::string s = r.stdoutText;
    if (!s.empty() && s.back() == '\n') {
        s.pop_back();
        if (!s.empty() && s.back() == '\r') s.pop_back();
    }
    return s;
}

// The FACTORY-RESULT (stub) shape's source: `FrozenArray<int>.From(a)`'s
// result carries `builtinClassSym("FrozenArray")`, NOT a written-out
// annotation. stdlib §4.19 declares `public class FrozenArray<T>:
// IEnumerable<T>` — its only interface — and its instance surface is
// `Length`, the indexer `[i: int]: T`, `ToArray()` and `GetEnumerator()`: it
// declares NO `Count` (that is §4.4's `Array<T>` member) and no `Add`.
const std::string kStubSource =
    "    using var a: Array<int> := [1, 2, 1];\n"
    "    using var f := FrozenArray<int>.From(a);\n";

// The ANNOTATION shape's source — the SAME type, written out.
const std::string kAnnotatedSource =
    "    using var a: Array<int> := [1, 2, 1];\n"
    "    using var f: FrozenArray<int> := FrozenArray<int>.From(a);\n";

}  // namespace

// ══════════════════════ #8350 — the §4.19 head's bare-reference row ══════

// The raise row: a bare reference to `Count` on a `FrozenArray<T>`. stdlib
// §4.19 declares no `Count`, so the reference must be REFUSED with the
// stdlib's documented-surface diagnostic.
//
// WITHOUT the fix this program COMPILED CLEAN, read `3` and exited 0 (the
// stub shape) — a silent over-acceptance of a member the standard library
// does not declare.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           frozenarray_stub_bare_count_rejected_stdlib_4_19) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kStubSource +
        "    var n := f.Count;\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        hasNoMemberDiagnostic(r, "Count", "FrozenArray"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.19"));
}

// The ANNOTATED twin of the very same type and the very same member: the two
// shapes of one type must agree about one head's surface.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           frozenarray_annotated_bare_count_rejected_stdlib_4_19) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kAnnotatedSource +
        "    var n := f.Count;\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        hasNoMemberDiagnostic(r, "Count", "FrozenArray"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.19"));
}

// The issue's OTHER repro: a bare reference to a member NO section declares.
// The stub shape must refuse it too (it used to compile clean, warn about the
// codegen fall-through and degrade the slot to zero — #2434/#2547).
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           frozenarray_stub_bare_bogus_member_rejected_stdlib_4_19) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kStubSource +
        "    var n := f.BogusMember;\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        hasNoMemberDiagnostic(r, "BogusMember", "FrozenArray"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.19"));
}

// The quoted list for the §4.19 head is GENERATED from that head's row: it
// carries `Length`, `ToArray` and `GetEnumerator` — and NOT `Count` (that is
// §4.4's `Array<T>` member).
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           frozenarray_quoted_surface_is_the_stdlib_4_19_surface) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kStubSource +
        "    var n := f.Count;\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    const std::string surface = quotedSurface(r);
    BEZEL_EXPECT_TRUE(!surface.empty());
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Length"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "ToArray"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "GetEnumerator"));
    BEZEL_EXPECT_FALSE(quotedSurfaceHas(surface, "Count"));
    BEZEL_EXPECT_FALSE(quotedSurfaceHas(surface, "Add"));
}

// The declared members must KEEP working in the bare form (the #2536
// catch-all's own constraint: over-rejecting a documented member is the
// failure mode the standing protocol weights most heavily against).
// `Length` is declared by stdlib §4.19.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           frozenarray_stub_bare_length_admitted_stdlib_4_19) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kStubSource +
        "    var n := f.Length;\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(printed(r) == "3");
}

// `ToArray` is declared by stdlib §4.19.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           frozenarray_stub_bare_toarray_admitted_stdlib_4_19) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kStubSource +
        "    var t := f.ToArray;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(hasNoMemberDiagnostic(r, "ToArray", "FrozenArray"));
}

// `GetEnumerator` is declared by stdlib §4.19.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           frozenarray_stub_bare_getenumerator_admitted_stdlib_4_19) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kStubSource +
        "    var e := f.GetEnumerator;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(
        hasNoMemberDiagnostic(r, "GetEnumerator", "FrozenArray"));
}

// The INDEXER `f[0]` is declared by stdlib §4.19 and must keep working.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           frozenarray_stub_indexer_admitted_stdlib_4_19) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kStubSource +
        "    Console.WriteLine(f[0]);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(printed(r) == "1");
}

// The CALL form of an undeclared member is UNCHANGED — `fix #8331` already
// owns it, and it must keep refusing the same name with the same wording.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           frozenarray_stub_call_bogus_member_still_rejected_stdlib_4_19) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n" +
        kStubSource +
        "    f.BogusMember();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        hasNoMemberDiagnostic(r, "BogusMember", "FrozenArray"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.19"));
}

// ═════════════ #8351 — the quoted list is GENERATED from the row ═════════

// stdlib §4.17 declares `Count`, `Add` and `TryAdd` on `SortedDictionary<K,V>`;
// the old hand-written list omitted all three, so the diagnostic told the user
// that members the standard library DOES declare were not part of the
// surface.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           sorteddictionary_quoted_surface_declares_add_count_tryadd) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var d := new SortedDictionary<int, int>();\n"
        "    var f := d.TotallyBogusMember;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string surface = quotedSurface(r);
    BEZEL_EXPECT_TRUE(!surface.empty());
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Add"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Count"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "TryAdd"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.17"));
}

// stdlib §4.8 declares `Count`, `Keys`, `Values` and `TryAdd` on
// `Dictionary<K,V>`; the old hand-written list omitted all four.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           dictionary_quoted_surface_declares_count_keys_values_tryadd) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var d := new Dictionary<int, int>();\n"
        "    var f := d.TotallyBogusMember;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string surface = quotedSurface(r);
    BEZEL_EXPECT_TRUE(!surface.empty());
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Count"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Keys"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Values"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "TryAdd"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.8"));
}

// stdlib §4.9 declares `Count`, `Keys` and `Values` on
// `OwningDictionary<K,V>`; the old hand-written list omitted all three.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           owningdictionary_quoted_surface_declares_count_keys_values) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class V public N: int := 0; "
        "public constructor(n: int) this.N := n; end constructor; end V;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var d := new OwningDictionary<int, V>();\n"
        "    var f := d.TotallyBogusMember;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string surface = quotedSurface(r);
    BEZEL_EXPECT_TRUE(!surface.empty());
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Count"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Keys"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Values"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.9"));
}

// stdlib §4.10's `Set<T>`/`OwningSet<T>` surface: `TryAdd` is declared, and
// `ContainsKey`/`TryGetValue`/`Drain` are NOT (they are map members).
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           set_quoted_surface_matches_stdlib_4_10) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var s := new Set<int>();\n"
        "    var f := s.TotallyBogusMember;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string surface = quotedSurface(r);
    BEZEL_EXPECT_TRUE(!surface.empty());
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Add"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Contains"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Count"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "TryAdd"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "UnionWith"));
    BEZEL_EXPECT_FALSE(quotedSurfaceHas(surface, "ContainsKey"));
    BEZEL_EXPECT_FALSE(quotedSurfaceHas(surface, "TryGetValue"));
    BEZEL_EXPECT_FALSE(quotedSurfaceHas(surface, "Drain"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.10"));
}

// stdlib §4.6's `LinkedList<T>` surface: the list members, and NO map
// members (`ContainsKey`/`TryGetValue`/`GetOrAdd`).
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           linkedlist_quoted_surface_matches_stdlib_4_6) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var l := new LinkedList<int>();\n"
        "    var f := l.TotallyBogusMember;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string surface = quotedSurface(r);
    BEZEL_EXPECT_TRUE(!surface.empty());
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "AddFirst"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "AddLast"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "RemoveFirst"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Find"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Count"));
    BEZEL_EXPECT_FALSE(quotedSurfaceHas(surface, "ContainsKey"));
    BEZEL_EXPECT_FALSE(quotedSurfaceHas(surface, "GetOrAdd"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.6"));
}

// stdlib §4.18's `SortedSet<T>` surface: `First`/`Last` are declared, and
// `UnionWith` is NOT (it is §4.10's `Set<T>` member).
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           sortedset_quoted_surface_matches_stdlib_4_18) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var s := new SortedSet<int>();\n"
        "    var f := s.TotallyBogusMember;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string surface = quotedSurface(r);
    BEZEL_EXPECT_TRUE(!surface.empty());
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Add"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "First"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "Last"));
    BEZEL_EXPECT_TRUE(quotedSurfaceHas(surface, "TryAdd"));
    BEZEL_EXPECT_FALSE(quotedSurfaceHas(surface, "UnionWith"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.18"));
}

// ROW 10 — THE LIST AND THE DECISION COME FROM THE SAME ROW.
//
// (a) A name the head does NOT declare is refused as a non-member, and the
//     quoted list does not carry it: `Set<T>` declares no `ContainsKey`
//     (stdlib §4.10), so the diagnostic must not quote it.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           set_containskey_is_not_a_member_and_is_not_quoted) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var s := new Set<int>();\n"
        "    var f := s.ContainsKey;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        hasNoMemberDiagnostic(r, "ContainsKey", "Set"));
    BEZEL_EXPECT_FALSE(quotedSurfaceHas(quotedSurface(r), "ContainsKey"));
}

// (b) The sibling: `SortedDictionary<K,V>` declares no `GetOrAdd`
//     (stdlib §4.17) — the old hand-written FAMILY UNION admitted it
//     because `Dictionary<K,V>` declares it.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           sorteddictionary_getoradd_is_not_a_member_and_is_not_quoted) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var d := new SortedDictionary<int, int>();\n"
        "    var f := d.GetOrAdd;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        hasNoMemberDiagnostic(r, "GetOrAdd", "SortedDictionary"));
    BEZEL_EXPECT_FALSE(
        quotedSurfaceHas(quotedSurface(r), "GetOrAdd"));
}

// (c) The other direction: a name the head DOES declare is NOT refused as a
//     non-member. `SortedDictionary<K,V>` declares `TryAdd` (stdlib §4.17),
//     so the "no member" diagnostic must not fire for it — and the §4.17
//     section it is decided against is the one the diagnostic cites.
BEZEL_TEST(collection_bare_reference_surface_8350_8351,
           sorteddictionary_tryadd_is_a_declared_member) {
    JitResult r = runAot(
        "module M; import System; import System.Collections.Generic;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    using var d := new SortedDictionary<int, int>();\n"
        "    var f := d.TryAdd;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(
        hasNoMemberDiagnostic(r, "TryAdd", "SortedDictionary"));
    BEZEL_EXPECT_TRUE(citesStdlibSection(r, "4.17"));
}
