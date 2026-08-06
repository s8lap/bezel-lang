// A SNAPSHOT of every AST-keyed side table on `sema::SemaContext`, so that
// adding a seventh one is a loud test failure rather than a silent new bug.
//
// WHY THIS EXISTS.  §2.5/§14.1 body reconstruction re-parses an imported
// generic member's body into a FRESH SemaContext and then carries the results
// back into the importer's context.  Anything the sema pass recorded in a
// table keyed on an AST node must be carried too — and every table that was
// NOT carried has produced a silent wrong value:
//
//   #2818  `exprSymbols_`   LocalSymbol dropped   -> every local read = 0
//   #2839  `lambdaParams_`  never copied          -> every lambda = 0
//   #2856  (same, static field initializer path)  -> silently 0
//   #2855  (same, destructor path)                -> SIGSEGV in the compiler
//   #2857  `patternSymbols_` carried nowhere      -> mislocated compile error
//
// Each was found one at a time, by someone tripping over it. The #2839 lane
// enumerated the tables and closed the question *for the class as it stands
// today* — but noted the gap this test fills: nothing mechanically ties "a new
// table keyed on an AST node" to "the reconstruction loops must decide about
// it", so a seventh table would silently re-acquire the whole bug class.
//
// WHAT THIS TEST DOES.  It scans `src/sema/SemaContext.hpp` for declarations of
// the form `std::unordered_map<const ast::…> name_;` and asserts the set of
// member names matches the list below.  It deliberately does NOT check that a
// table is carried — that is a judgement each table needs individually, and
// several are correctly not carried.  It only forces the judgement to be MADE.
//
// ⚠ IF THIS TEST FAILS because you added a table: do not just add the name.
// Decide, and record the decision here, whether
// `ModuleLoader::parseAndAttach{MethodBody,CtorBody,DtorBody,…}` must carry it.
// If it must, carry it in the same commit — that is the whole point.
//
// Matching is on the member NAME, which is stable under reformatting; the
// scan tolerates a declaration wrapped across lines (`exprExtBindings_` is).

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_harness.hpp"

namespace {

// The six tables as of v0.240, each with the disposition the module loader's
// reconstruction takes. Kept beside the names deliberately: the disposition is
// the part a reader needs when this test fails.
struct Entry {
    const char* name;
    const char* disposition;
};
const Entry kAstKeyedTables[] = {
    {"resolvedTypes_",
     "CARRIED verbatim by all four paths (method/ctor/field-init; dtor #2855)"},
    {"exprTypes_",
     "CARRIED verbatim by all four paths (method/ctor/field-init; dtor #2855)"},
    {"exprSymbols_",
     "REMAPPED per symbol kind; Local carried (#2290 ctor, #2818 method, "
     "#2855 dtor), lambda Parameter carried (#2839 method+ctor, #2855 dtor; "
     "field-init carries it via #2375's verbatim else)"},
    {"patternSymbols_",
     "CARRIED NOWHERE - reachable and broken, tracked by #2857"},
    {"lambdaParams_",
     "CARRIED verbatim by all four paths: method + ctor (#2839), "
     "field-init (#2856), dtor (#2855)"},
    {"exprExtBindings_",
     "NOT carried, and proven UNREACHABLE: a body naming an extension method "
     "declines reconstruction at #783's gate before this table is consulted"},
};

// Locate the repo root from this translation unit's own path:
// <root>/tests/module/<this file>.
std::string repoRoot() {
    std::string p = __FILE__;
    for (int up = 0; up < 3; ++up) {
        std::size_t slash = p.find_last_of('/');
        if (slash == std::string::npos) return {};
        p.resize(slash);
    }
    return p;
}

// Every member name declared as `std::unordered_map<const ast::…> name_;`,
// in declaration order. Scans forward from each match to the terminating `;`
// so a declaration wrapped across lines is handled.
std::vector<std::string> scanAstKeyedTableNames(const std::string& src) {
    std::vector<std::string> out;
    const std::string needle = "std::unordered_map<const ast::";
    for (std::size_t at = src.find(needle); at != std::string::npos;
         at = src.find(needle, at + 1)) {
        // Skip accessor return types in the public section: those are preceded
        // by `const ` and followed by `&`, and declare no member.
        std::size_t semi = src.find(';', at);
        if (semi == std::string::npos) break;
        std::string decl = src.substr(at, semi - at);
        if (decl.find('&') != std::string::npos) continue;  // accessor signature
        // The member name is the last identifier before the `;`.
        std::size_t end = decl.find_last_not_of(" \t\r\n");
        if (end == std::string::npos) continue;
        std::size_t begin = end;
        while (begin > 0 &&
               (std::isalnum(static_cast<unsigned char>(decl[begin - 1])) ||
                decl[begin - 1] == '_'))
            --begin;
        std::string name = decl.substr(begin, end - begin + 1);
        if (!name.empty() && name.back() == '_') out.push_back(name);
    }
    return out;
}

} // namespace

BEZEL_TEST(semacontext_ast_side_table_snapshot,
           every_ast_keyed_table_has_a_recorded_reconstruction_disposition) {
    const std::string root = repoRoot();
    BEZEL_EXPECT_TRUE(!root.empty());
    const std::string header = root + "/src/sema/SemaContext.hpp";

    std::ifstream in(header);
    BEZEL_EXPECT_TRUE(in.good());  // header findable from __FILE__
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::vector<std::string> found = scanAstKeyedTableNames(ss.str());

    // Every declared table must be in the snapshot.
    for (const std::string& name : found) {
        bool known = false;
        for (const Entry& e : kAstKeyedTables)
            if (name == e.name) known = true;
        if (!known) {
            std::cout << "    NEW AST-keyed side table on SemaContext: " << name
                      << "\n    Decide whether ModuleLoader's body"
                         " reconstruction must carry it (see #2818/#2839),"
                         " then add it to kAstKeyedTables with that"
                         " disposition.\n";
        }
        BEZEL_EXPECT_EQ(known, true);
    }

    // And every snapshot entry must still exist — a removed table should
    // retire its entry, not leave a stale claim about the loader's behaviour.
    for (const Entry& e : kAstKeyedTables) {
        bool present = false;
        for (const std::string& name : found)
            if (name == e.name) present = true;
        if (!present)
            std::cout << "    snapshot lists a table that no longer exists: "
                      << e.name << " (" << e.disposition << ")\n";
        BEZEL_EXPECT_EQ(present, true);
    }

    BEZEL_EXPECT_EQ(found.size(),
                    sizeof(kAstKeyedTables) / sizeof(kAstKeyedTables[0]));
}
