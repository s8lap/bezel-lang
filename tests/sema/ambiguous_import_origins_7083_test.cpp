// fix #7083 / §2.2 (the "Collisions" paragraph), v0.343.
//
// THE RULE. §2.2 (v0.343):
//
//   "When two imported modules export the same public name, the **bare** name
//    is ambiguous: using it unqualified is a compile error citing both
//    origins; the fix is qualification (full module name or alias)."
//
// "CITING BOTH ORIGINS" is the part a diagnostic has to discharge: the
// message must name the modules the two colliding exports come from, so the
// reader knows which two imports to qualify or alias. A message that says
// only "multiple imported modules" names the rule and not the origins.
//
// THE DEFECT. The diagnostic #7013's fix introduced for the arity-0 chain-head
// selection read:
//
//   "bare name `Zed` is ambiguous: multiple imported modules export a type
//    with this name and arity; qualify it via a module-qualified name or an
//    import alias (§2.2)"
//
// — the rule, and no origins. The helper that produces it
// (`selectImportedTypeByArity` in `src/sema/TypeResolution_Names.cpp`) HAS
// both module names in hand and dropped them.
//
// WHAT THIS ADDS. The origin modules' names are in the message, and they are
// in a DETERMINISTIC order: the helper walks `sourceImports_`, an
// `unordered_set`, so the order in which its members are visited is not a
// property to build a diagnostic's text on. The names are sorted before they
// are written, so the message is byte-identical from run to run.
//
// SCOPE — what is deliberately NOT claimed, and why:
//   * The PRE-EXISTING sibling diagnostic (the §2.2 collision reported when
//     nothing in scope binds the name, `src/sema/TypeResolution_Names.cpp`'s
//     `ambiguousImportNames_` arm) still reads "multiple imported modules".
//     §2.2's requirement reaches it too, but naming its origins means
//     recording them where the import loop populates the collision set
//     (`src/sema/TypeResolution.cpp`), which is outside this fix's file. The
//     deferral is named in the commit body, not left silent.
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

// A library exporting a public, NON-GENERIC `C` with a public nested `N`.
// The nested `N` is what the chain head reaches, so the reference under test
// is a CHAIN HEAD (`C.N`) — the shape §11.6 calls "a bare enclosing segment"
// and #7013's fix made select an imported arity-0 type.
std::string libWithNongenericC(const std::string& moduleName, int value) {
    return "module " + moduleName + ";\n"
           "public class C\n"
           "  public class N\n"
           "    public method Get(): int return " + std::to_string(value) +
           "; end Get;\n"
           "  end N;\n"
           "end C;\n";
}

// A library exporting a public GENERIC `C<T>` with a public nested `N`.
std::string libWithGenericC(const std::string& moduleName) {
    return "module " + moduleName + ";\n"
           "public class C<T>\n"
           "  public class N\n"
           "    public method Get(): int return 5; end Get;\n"
           "  end N;\n"
           "end C;\n";
}

// The app's own generic `C<T>`, which §2.2 says does not shadow an imported
// non-generic `C` (the two are names of DIFFERENT arity).
constexpr const char* kAppBody = R"(
class C<T>
  public method Id(x: T): T return x; end Id;
end C;
class D
  public static method Main(): int
    using var n: C.N := new C.N();
    Console.WriteLine(n.Get());
    return 0;
  end Main;
end D;
)";

std::string appWith(const std::string& moduleName,
                    const std::vector<std::string>& imports) {
    std::string src = "module " + moduleName + ";\n";
    for (const std::string& imp : imports) src += "import " + imp + ";\n";
    src += kAppBody;
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
// across runs (the determinism row).
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
// base the message read "multiple imported modules export a type with this
// name and arity" and named neither module.
BEZEL_TEST(ambiguous_import_origins_7083,
           ambiguous_head_cites_both_origins_spec_2_2) {
    const std::string app = appWith("AppA", {"LA", "LB"});
    JitResult r = runAotModules({ModuleSource{"LA", libWithNongenericC("LA", 21)},
                                 ModuleSource{"LB", libWithNongenericC("LB", 22)},
                                 ModuleSource{"AppA", app}},
                                "AppA");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(
        r, "is ambiguous: imported modules `LA` and `LB` export a type with "
           "this name and arity"));
    BEZEL_EXPECT_TRUE(hasErrorWith(r, "\xC2\xA7""2.2"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// §2.2 (v0.343): the origins' ORDER in the message must not depend on the
// `unordered_set` the helper walks — the same program must produce the same
// text, byte for byte, on every run. The row reads the diagnostic's text out
// of the result and compares it across runs; a row that only counted
// diagnostics could not see an unstable ORDER.
BEZEL_TEST(ambiguous_import_origins_7083,
           ambiguous_head_origins_are_deterministic_spec_2_2) {
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

// §2.2 (v0.343): "citing both origins" — with THREE colliding imports, every
// origin is cited, and they are cited in a deterministic (sorted) order.
BEZEL_TEST(ambiguous_import_origins_7083,
           three_colliding_imports_cite_every_origin_spec_2_2) {
    const std::string app = appWith("AppC", {"LA", "LB", "LC"});
    JitResult r = runAotModules({ModuleSource{"LA", libWithNongenericC("LA", 21)},
                                 ModuleSource{"LB", libWithNongenericC("LB", 22)},
                                 ModuleSource{"LC", libWithNongenericC("LC", 23)},
                                 ModuleSource{"AppC", app}},
                                "AppC");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorWith(
        r, "is ambiguous: imported modules `LA`, `LB` and `LC` export a type "
           "with this name and arity"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================================
// Part 2 — CONTROLS. The origin citation must not turn a NON-collision into
// one, and must not change a program that compiles.
// ============================================================================

// §2.2 (v0.343): the collision is at the (name, ARITY) pair — "names are
// compared together with their generic arity". Two imports exporting the same
// NAME at DIFFERENT arities are two names, "neither colliding with nor
// shadowing the other", so the chain head selects the arity-0 import and the
// program RUNS. No ambiguity, no origins.
BEZEL_TEST(ambiguous_import_origins_7083,
           different_arities_are_not_a_collision_spec_2_2) {
    const std::string app = appWith("AppD", {"LA", "LB"});
    JitResult r = runAotModules({ModuleSource{"LA", libWithNongenericC("LA", 21)},
                                 ModuleSource{"LB", libWithGenericC("LB")},
                                 ModuleSource{"AppD", app}},
                                "AppD");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "21\n");
}

// §2.2 (v0.343): ONE import exporting the type is not a collision at all —
// the chain head selects it and the program runs. The origin citation is
// reached only when there are two.
BEZEL_TEST(ambiguous_import_origins_7083,
           single_import_is_not_a_collision_spec_2_2) {
    const std::string app = appWith("AppE", {"LA"});
    JitResult r = runAotModules({ModuleSource{"LA", libWithNongenericC("LA", 21)},
                                 ModuleSource{"AppE", app}},
                                "AppE");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "21\n");
}

// §2.2 (v0.343): the fix is qualification, so the QUALIFIED spelling of each
// origin still reaches its own type and both programs run — the diagnostic
// recommends a remedy that works.
BEZEL_TEST(ambiguous_import_origins_7083,
           each_origin_stays_reachable_by_qualifier_spec_2_2) {
    const std::string appA = "module AppF;\n"
                             "import LA;\n"
                             "class D\n"
                             "  public static method Main(): int\n"
                             "    using var n: LA.C.N := new LA.C.N();\n"
                             "    Console.WriteLine(n.Get());\n"
                             "    return 0;\n"
                             "  end Main;\n"
                             "end D;\n";
    const std::string appB = "module AppG;\n"
                             "import LB;\n"
                             "class D\n"
                             "  public static method Main(): int\n"
                             "    using var n: LB.C.N := new LB.C.N();\n"
                             "    Console.WriteLine(n.Get());\n"
                             "    return 0;\n"
                             "  end Main;\n"
                             "end D;\n";
    JitResult ra = runAotModules({ModuleSource{"LA", libWithNongenericC("LA", 21)},
                                  ModuleSource{"AppF", appA}},
                                 "AppF");
    BEZEL_EXPECT_FALSE(ra.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(ra.ran);
    BEZEL_EXPECT_EQ(ra.stdoutText, "21\n");
    JitResult rb = runAotModules({ModuleSource{"LB", libWithNongenericC("LB", 22)},
                                  ModuleSource{"AppG", appB}},
                                 "AppG");
    BEZEL_EXPECT_FALSE(rb.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rb.ran);
    BEZEL_EXPECT_EQ(rb.stdoutText, "22\n");
}
