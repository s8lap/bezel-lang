// fix #7096 / §2.2 (the "Collisions" paragraph), v0.343.
//
// THE RULE. §2.2 (v0.343):
//
//   "When two imported modules export the same public name, the **bare** name
//    is ambiguous: using it unqualified is a compile error citing both
//    origins; the fix is qualification (full module name or alias)."
//
// "CITING BOTH ORIGINS" is the part a diagnostic has to discharge: the
// message must name the modules the two colliding exports come from, so the
// reader knows which two imports to qualify or alias.
//
// THE DEFECT. §2.2's collision has TWO report sites. The chain-head one (the
// `selectImportedTypeByArity` selection in `src/sema/TypeResolution_Names.cpp`)
// was fixed by #7083 and already names the origins. The OTHER one — the
// `ambiguousImportNames_` arm, the diagnostic a bare name gets when nothing in
// scope binds it — still read:
//
//   "bare name `C` is ambiguous: multiple imported modules export a type
//    with this name and arity; qualify it via a module-qualified name or an
//    import alias (§2.2)"
//
// — the rule, and NEITHER module. The collision table it reads
// (`Resolver::ambiguousImportNames_`) stored only the (name, arity) KEY, so
// the origin modules were not available to be cited.
//
// WHAT THIS ADDS. The origin modules' names are in the message, in a
// DETERMINISTIC order: they are sorted (and deduplicated) where the import
// loop records them, so the message is byte-identical from run to run and
// does not depend on the order the imports were visited in — the same
// guarantee the chain-head site gives.
//
// WHICH SITE THESE ROWS PIN. §2.2's two sites are reached by DIFFERENT
// reference shapes, and this file pins the SECOND one:
//
//   * site 1 (the chain-head selection) is reached by a MULTI-PART reference
//     (`C.N`) whose head is a GENERIC type in scope — the importing module
//     declares `class C<T>` itself. That is #7083's shape and #7083's rows
//     (`tests/sema/ambiguous_import_origins_7083_test.cpp`).
//   * site 2 (the `ambiguousImportNames_` arm) is reached when NOTHING in
//     scope binds the bare name: the importing module declares NO `C` of its
//     own, so the two colliding imports are the only candidates.
//
// The rows below reference the BARE `C` in a type position — a one-part
// reference, which never enters site 1's branch at all — and one row uses the
// issue's own CHAIN-HEAD spelling (`C.N`) with no local declaration, which
// reaches site 2 as well: site 1 needs a GENERIC head in scope, and the
// chain-head spelling alone does not supply one.
//
// SCOPE — what is deliberately NOT claimed, and why:
//   * The ORIGINS are recorded where the import loop already knows them
//     (`src/sema/TypeResolution.cpp`), NOT recomputed at the report site: that
//     loop applies the rules that decide which exports count as an origin at
//     all (a local declaration shadows the import, an always-available name is
//     excluded, a self-import is skipped), and a second copy of those rules
//     is a drift channel for a diagnostic that has to be exactly right.
//   * The collision is at the (name, ARITY) pair, not the bare name
//     (§2.2's own "a non-generic `Counter` and the always-available
//     `Counter<K>` are two names, neither colliding with nor shadowing the
//     other"), so two imports exporting the same NAME at DIFFERENT arities are
//     not a collision — pinned by a control row below.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::runAotModules;

namespace {

// A library exporting a public, NON-GENERIC `C` — one of the two colliding
// origins. The `C` is a plain class: the reference under test is the BARE
// name `C` in a type position, not a chain head.
std::string libWithNongenericC(const std::string& moduleName, int value) {
    return "module " + moduleName + ";\n"
           "public class C\n"
           "  public method Get(): int return " + std::to_string(value) +
           "; end Get;\n"
           "end C;\n";
}

// A library exporting a public GENERIC `C<T>` — a DIFFERENT name from the
// non-generic `C` (§2.2 compares names "together with their generic arity").
std::string libWithGenericC(const std::string& moduleName) {
    return "module " + moduleName + ";\n"
           "public class C<T>\n"
           "  public method Get(): int return 5; end Get;\n"
           "end C;\n";
}

// A library exporting a public, NON-GENERIC `C` with a public nested `N` —
// the shape the ISSUE's own repro uses: the reference is the CHAIN HEAD
// `C.N`, and the nested `N` is what the chain reaches.
std::string libWithNongenericCAndNestedN(const std::string& moduleName,
                                          int value) {
    return "module " + moduleName + ";\n"
           "public class C\n"
           "  public class N\n"
           "    public method Get(): int return " + std::to_string(value) +
           "; end Get;\n"
           "  end N;\n"
           "end C;\n";
}

// The importing module. It declares NO `C` of its own — that is what makes
// the colliding imports the only candidates, and what keeps site 1 (the
// chain-head selection) out of reach — and it references the bare `C` in a
// type position (a parameter's type), which is a ONE-PART reference.
std::string appWith(const std::string& moduleName,
                    const std::vector<std::string>& imports) {
    std::string src = "module " + moduleName + ";\n";
    for (const std::string& imp : imports) src += "import " + imp + ";\n";
    src +=
        "class D\n"
        "  public method Take(c: C): int return 0; end Take;\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(1);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end D;\n";
    return src;
}

bool hasErrorWith(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The FIRST error message, so a row can compare the text byte-for-byte
// across runs (the determinism rows).
std::string firstErrorMessage(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) return d.message;
    return std::string();
}

// The harness prefixes each diagnostic with the source file it came from —
// a per-run TEMPORARY path — so the message's own text is compared from the
// first word of the diagnostic's TEXT onwards, and only to the end of that
// line. The part before it (and any following line) is the harness's
// location prefix, not the compiler's wording.
std::string diagnosticText(const std::string& message) {
    const std::size_t k = message.find("is ambiguous:");
    if (k == std::string::npos) return message;
    const std::size_t eol = message.find('\n', k);
    return eol == std::string::npos ? message.substr(k)
                                     : message.substr(k, eol - k);
}

}  // namespace

// ============================================================================
// Part 1 — the defect. §2.2 requires the collision diagnostic to cite BOTH
// ORIGINS; the base named the rule and not the modules.
// ============================================================================

// §2.2 (v0.343): two DISTINCT imports exporting the same (name, arity) make
// the bare name ambiguous, and the diagnostic must cite both origins. On the
// base this site read "multiple imported modules export a type with this name
// and arity" and named neither module.
BEZEL_TEST(ambiguous_import_origins_7096,
           ambiguous_bare_name_cites_both_origins_spec_2_2) {
    const std::string app = appWith("AppA", {"LA", "LB"});
    JitResult r = runAotModules({ModuleSource{"LA", libWithNongenericC("LA", 21)},
                                 ModuleSource{"LB", libWithNongenericC("LB", 22)},
                                 ModuleSource{"AppA", app}},
                                "AppA");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The origins are named, in the sorted order "`LA` and `LB`".
    BEZEL_EXPECT_TRUE(hasErrorWith(
        r, "is ambiguous: imported modules `LA` and `LB` export a type with "
           "this name and arity"));
    // The remedy §2.2 names is still offered.
    BEZEL_EXPECT_TRUE(hasErrorWith(
        r, "qualify it via a module-qualified name or an import alias"));
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "\xC2\xA7""2.2"));
    // The base's wording — the rule and NEITHER origin — is gone.
    BEZEL_EXPECT_FALSE(hasErrorWith(
        r, "multiple imported modules export a type with this name and arity"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.2 (v0.343): the origins' ORDER in the message must not depend on the
// order the imports were visited in — the same program must produce the same
// text, byte for byte, on every run. The row reads the diagnostic's text out
// of the result and compares it across runs; a row that only counted
// diagnostics could not see an unstable ORDER.
BEZEL_TEST(ambiguous_import_origins_7096,
           ambiguous_bare_name_origins_are_deterministic_spec_2_2) {
    const std::string app = appWith("AppB", {"LA", "LB"});
    std::string first;
    for (int i = 0; i < 8; ++i) {
        JitResult r = runAotModules({ModuleSource{"LA", libWithNongenericC("LA", 21)},
                                     ModuleSource{"LB", libWithNongenericC("LB", 22)},
                                     ModuleSource{"AppB", app}},
                                    "AppB");
        BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
        const std::string msg = firstErrorMessage(r);
        BEZEL_EXPECT_TRUE(msg.find("imported modules `LA` and `LB`") !=
                          std::string::npos);
        const std::string text = diagnosticText(msg);
        if (i == 0) {
            first = text;
        } else {
            BEZEL_EXPECT_EQ(text, first);
        }
    }
}

// §2.2 (v0.343): the origins are cited in a SORTED order, so the order the
// imports are WRITTEN in does not change the message. This program imports
// `LB` first and `LA` second — the opposite of the row above — and the
// message must still read "`LA` and `LB`".
BEZEL_TEST(ambiguous_import_origins_7096,
           ambiguous_bare_name_origin_order_is_sorted_spec_2_2) {
    const std::string app = appWith("AppC", {"LB", "LA"});
    JitResult r = runAotModules({ModuleSource{"LA", libWithNongenericC("LA", 21)},
                                 ModuleSource{"LB", libWithNongenericC("LB", 22)},
                                 ModuleSource{"AppC", app}},
                                "AppC");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(
        r, "is ambiguous: imported modules `LA` and `LB` export a type with "
           "this name and arity"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.2 (v0.343): "citing both origins" — with THREE colliding imports, every
// origin is cited, and they are cited in a deterministic (sorted) order.
BEZEL_TEST(ambiguous_import_origins_7096,
           three_colliding_imports_cite_every_origin_spec_2_2) {
    const std::string app = appWith("AppD", {"LA", "LB", "LC"});
    JitResult r = runAotModules({ModuleSource{"LA", libWithNongenericC("LA", 21)},
                                 ModuleSource{"LB", libWithNongenericC("LB", 22)},
                                 ModuleSource{"LC", libWithNongenericC("LC", 23)},
                                 ModuleSource{"AppD", app}},
                                "AppD");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(
        r, "is ambiguous: imported modules `LA`, `LB` and `LC` export a type "
           "with this name and arity"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.2 (v0.343): the ISSUE's own repro shape — a CHAIN HEAD (`C.N`) whose
// head has NO local declaration. The chain-head SPELLING does not make this
// site 1: site 1 is reached only when a GENERIC head is in scope (the
// importing module declares `class C<T>` itself — #7083's shape, whose rows
// live in `tests/sema/ambiguous_import_origins_7083_test.cpp`). With no local
// `C` at all the two colliding imports are the only candidates, so this
// reaches the same site 2, and the message names both origins.
//
// This row is also the pin that the two sites' POPULATIONS agree: the import
// loop skips an imported type whose bare name a LOCAL declaration shadows,
// so a local `C<T>` keeps the imported `C` out of the collision table
// entirely — and that is why site 1 is the site that fires there.
BEZEL_TEST(ambiguous_import_origins_7096,
           ambiguous_chain_head_without_a_local_declaration_cites_both_origins_spec_2_2) {
    const std::string app = "module AppI;\n"
                             "import LA;\n"
                             "import LB;\n"
                             "class D\n"
                             "  public static method Main(): int\n"
                             "    using var n: C.N := new C.N();\n"
                             "    Console.WriteLine(n.Get());\n"
                             "    return 0;\n"
                             "  end Main;\n"
                             "end D;\n";
    JitResult r = runAotModules(
        {ModuleSource{"LA", libWithNongenericCAndNestedN("LA", 21)},
         ModuleSource{"LB", libWithNongenericCAndNestedN("LB", 22)},
         ModuleSource{"AppI", app}},
        "AppI");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(
        r, "is ambiguous: imported modules `LA` and `LB` export a type with "
           "this name and arity"));
    BEZEL_EXPECT_FALSE(hasErrorWith(
        r, "multiple imported modules export a type with this name and arity"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================================
// Part 2 — CONTROLS. The origin citation must not turn a NON-collision into
// one, and must not change a program that compiles.
// ============================================================================

// §2.2 (v0.343): the collision is at the (name, ARITY) pair — "names are
// compared together with their generic arity". Two imports exporting the same
// NAME at DIFFERENT arities are two names, "neither colliding with nor
// shadowing the other", so the bare `C` selects the arity-0 import and the
// program RUNS. No ambiguity, no origins.
BEZEL_TEST(ambiguous_import_origins_7096,
           different_arities_are_not_a_collision_spec_2_2) {
    const std::string app = appWith("AppE", {"LA", "LB"});
    JitResult r = runAotModules({ModuleSource{"LA", libWithNongenericC("LA", 21)},
                                 ModuleSource{"LB", libWithGenericC("LB")},
                                 ModuleSource{"AppE", app}},
                                "AppE");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// §2.2 (v0.343): ONE import exporting the type is not a collision at all —
// the bare name resolves to it and the program runs. The origin citation is
// reached only when there are two.
BEZEL_TEST(ambiguous_import_origins_7096,
           single_import_is_not_a_collision_spec_2_2) {
    const std::string app = appWith("AppF", {"LA"});
    JitResult r = runAotModules({ModuleSource{"LA", libWithNongenericC("LA", 21)},
                                 ModuleSource{"AppF", app}},
                                "AppF");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// §2.2 (v0.343): the fix is qualification, so the QUALIFIED spelling of each
// origin still reaches its own type and both programs run — the diagnostic
// recommends a remedy that works.
BEZEL_TEST(ambiguous_import_origins_7096,
           each_origin_stays_reachable_by_qualifier_spec_2_2) {
    const std::string appA = "module AppG;\n"
                             "import LA;\n"
                             "class D\n"
                             "  public method Take(c: LA.C): int return 0; "
                             "end Take;\n"
                             "  public static method Main(): int\n"
                             "    Console.WriteLine(1);\n"
                             "    return 0;\n"
                             "  end Main;\n"
                             "end D;\n";
    const std::string appB = "module AppH;\n"
                             "import LB;\n"
                             "class D\n"
                             "  public method Take(c: LB.C): int return 0; "
                             "end Take;\n"
                             "  public static method Main(): int\n"
                             "    Console.WriteLine(1);\n"
                             "    return 0;\n"
                             "  end Main;\n"
                             "end D;\n";
    JitResult ra = runAotModules({ModuleSource{"LA", libWithNongenericC("LA", 21)},
                                  ModuleSource{"AppG", appA}},
                                 "AppG");
    BEZEL_EXPECT_FALSE(ra.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(ra.ran);
    BEZEL_EXPECT_EQ(ra.stdoutText, "1\n");
    JitResult rb = runAotModules({ModuleSource{"LB", libWithNongenericC("LB", 22)},
                                  ModuleSource{"AppH", appB}},
                                 "AppH");
    BEZEL_EXPECT_FALSE(rb.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rb.ran);
    BEZEL_EXPECT_EQ(rb.stdoutText, "1\n");
}
