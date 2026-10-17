// fix #6373 (§2.2, §2.3, §2.7 rule 1, §2.7 rule 2, §11.6, §12.9, v0.338):
// a head's variance is read only off the type the NAME RESOLVER binds for that
// exact spelling — the qualifier decides which scope answers, and the rest of
// the path is followed to its end.
//
// §2.7 rule 1 resolves the leftmost segment of a dotted type name FIRST, as a
// bare name: local scope, then the enclosing type's accessible members, then the
// current module's declarations, then unambiguous imported bare names, then
// import aliases. Only if it binds to a TYPE does the rest of the chain resolve
// against that type (§11.6 nested types); only if it binds to a MODULE does the
// rest name a member of that module (§2.7 rule 2).
//
// `Resolver::checkVariancePositions` (src/sema/TypeResolution_Checks.cpp)
// answered all four of its arms from the spelling's LAST path segment, with no
// test on whether the spelling was qualified at all. Four false resolves, one
// cause, MEASURED here as four separate rows:
//
//   1. module-qualified `Lib.Outer.Box<T>` matched `Lib`'s top-level `Box` on
//      `parts.back()` alone, so the §2.7 rejection on the missing enclosing
//      type carried a second, spurious §12.9 diagnostic naming a type the
//      spelling never names. Alias-qualified `L.Outer.Box<T>` the same way
//      (#6373's body, plus its alias spelling).
//   2. a MULTI-SEGMENT module name (`import Com.Sub;` then `Com.Sub.Box<T>`)
//      was never found at all — `parts.front()` is `Com`, and
//      `sourceImports_` is keyed by the full dotted name — so a real §12.9
//      violation was SILENTLY ACCEPTED.
//   3. a LOCAL ALIAS sharing a module's name slipped past the shadowing guard,
//      which scanned `localTypes` (`TypeSymbol`s) and never `localAliases`.
//   4. a QUALIFIED spelling was answered off the LOCAL top-level table when the
//      file happened to declare a same-named type — rejecting a legal program
//      in one marker configuration and skipping a real violation in the other.
//   5. the built-in table answered a qualified spelling too, so `LibX.Array<T>`
//      earned a §12.9 diagnostic byte-identical to the bare `Array<T>` one.
//
// The CONTROLS are the other half of every pair and are what make the reject
// rows discriminating: a fix that simply stopped resolving qualified heads would
// make the "skips"/"does not reject" rows green while turning the nested-import
// row, the two local-vs-import rows and both built-in rows RED. A fix that
// narrowed to "exactly two path parts" would pass the skip rows and fail
// `nested_imported_head_still_rejected`.
//
// Every row is two-module (real Driver::compileObject + compileFileForTest with
// a Dependency) except the two that need no library at all, and the library
// builds in every row that has one — the defect is in the IMPORTER's
// declaration, not in the export.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

// §12.9's diagnostic, quoted whole: the head is named only by the composed
// clause, so a row that wants "§12.9 contributed nothing" has to look for the
// section mark and the composed clause together.
const char* kVariance129 = "(§12.9)";
const char* kComposedClause = "composed through a nested type constructor";

void write6373(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Result6373 {
    bool libBuilt = false;
    bool appCompiled = false;
    std::string err;
};

// `libSrc` empty means "single module": nothing is compiled to an object and
// the app is compiled on its own, for the rows whose whole point is a
// stdlib/built-in spelling.
Result6373 build6373(const std::string& libSrc, const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    const std::string id = unique_stem("bz6373_");
    const std::string appF = (dir / (id + "_app.bzl")).string();
    const std::string exeF = (dir / id).string();
    write6373(appF, appSrc);
    Result6373 r;
    r.libBuilt = true;
    std::vector<bezel::Dependency> deps;
    std::string libO;
    if (!libSrc.empty()) {
        const std::string libF = (dir / (id + "_lib.bzl")).string();
        libO = (dir / (id + "_lib.o")).string();
        write6373(libF, libSrc);
        bezel::Driver libDriver;
        r.libBuilt = libDriver.compileObject(libF, libO).success &&
                     std::ifstream(libO).good();
        if (!r.libBuilt) {
            std::remove(libF.c_str());
            std::remove(appF.c_str());
            return r;
        }
        bezel::Dependency d;
        d.metadataPath = libO;
        d.objectPath = libO;
        deps.push_back(d);
    }
    bezel::Driver driver;
    const std::string errPath = (dir / (id + "_err.txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    const int saved = ::dup(STDERR_FILENO);
    const int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    r.appCompiled =
        bezel::test::compileFileForTest(driver, appF, exeF, deps).success;
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    {
        std::ifstream in(errPath);
        r.err.assign((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
        std::remove(errPath.c_str());
    }
    std::remove(exeF.c_str());
    std::remove(appF.c_str());
    if (!libO.empty()) std::remove(libO.c_str());
    return r;
}

std::string lib6373(const std::string& decl) { return "module Lib; " + decl; }

// A library exporting one CONTRAVARIANT head, spelled the way each app names
// it.
std::string contraHead(const std::string& name) {
    return "public interface " + name + "<in T> method Put(x: T); end " + name +
           "; ";
}

// ... and its COVARIANT mirror, so a row can turn the polarity round without
// changing anything else about the file.
std::string coHead(const std::string& name) {
    return "public interface " + name + "<out T> method Get(): T; end " + name +
           "; ";
}

std::string app6373(const std::string& importLine, const std::string& decls) {
    return "module App; import System; " + importLine + decls +
           "public class Program "
           "  public static method Main(): int "
           "    Console.WriteLine(\"x\"); return 0; "
           "  end Main; "
           "end Program; ";
}

// --- 1. the module-qualified spelling whose enclosing path does not exist --
//
// `Lib` exports a top-level `Box<in T>` and NO `Outer`. `Lib.Outer.Box<T>` is
// not a name the language has: §2.7 rule 2 resolves `Lib` to the module and
// then `Outer` to nothing, so the name resolver rejects the program. §12.9 must
// contribute NOTHING to that rejection — reading `Lib.Box`'s marker would name a
// type the spelling never reaches.

BEZEL_TEST(head_qualifier_6373,
           module_qualified_missing_enclosing_type_skips_variance) {
    auto r = build6373(lib6373(contraHead("Box")),
                       app6373("import Lib; ",
                               "public interface I<out T> "
                               "  method Get(): Lib.Outer.Box<T>; "
                               "end I; "));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(r.err.find("module `Lib` has no public type `Outer`") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.err.find(kVariance129) == std::string::npos);
}

// The same shape through an import ALIAS. §2.2: an alias is a qualifier, not a
// rename, so `L.Outer.Box<T>` is the same unresolvable name and must be
// rejected by the same rule with the same silence from §12.9.

BEZEL_TEST(head_qualifier_6373,
           alias_qualified_missing_enclosing_type_skips_variance) {
    auto r = build6373(lib6373(contraHead("Box")),
                       app6373("import Lib as L; ",
                               "public interface I<out T> "
                               "  method Get(): L.Outer.Box<T>; "
                               "end I; "));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(r.err.find("module `Lib` has no public type `Outer`") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.err.find(kVariance129) == std::string::npos);
}

// --- the CONTROL that makes the two rows above worth having ----------------
//
// Byte-identical in shape to `module_qualified_missing_enclosing_type_skips_`:
// the same three segments, the same `out T`, the same contravariant head. The
// ONLY difference is that `Lib` really exports `Outer` with `Box<in T>` NESTED
// in it — so the spelling is a real §11.6 nested type and the §12.9 rejection
// is correct and required. A fix that answered "not two parts -> skip", which
// is the remedy #6373's body proposed and the one this lane declined, turns
// this row green-while-losing-a-rejection and leaves both rows above still
// broken by making them pass vacuously.

BEZEL_TEST(head_qualifier_6373, nested_imported_head_still_rejected) {
    auto r = build6373(
        lib6373("public class Outer "
                "  public interface Box<in T> method Put(x: T); end Box; "
                "end Outer; "),
        app6373("import Lib; ",
                "public interface I<out T> "
                "  method Get(): Lib.Outer.Box<T>; "
                "end I; "));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(r.err.find(kVariance129) != std::string::npos);
    BEZEL_EXPECT_TRUE(r.err.find(kComposedClause) != std::string::npos);
}

// --- the nested walk must land on the NESTED head, not on the enclosing one --
//
// The two rows above and here are the same spelling with the nested head's
// polarity flipped, and that is what pins WHERE the nested walk stops.
//
// `nested_imported_head_still_rejected` alone does not: an implementation that
// resolved `Lib.Outer.Box<T>` to the enclosing `Outer` CLASS would read
// `invariantOrDefer(Member)` -> `Invariant`, which rejects `out T` at a return
// slot just as `Box<in T>` does. That mutant was MEASURED green on that row
// (12 passed, 0 failed — a zero-change revert, i.e. a failed mutation). With
// the nested head COVARIANT the two readings part: the nested walk gives
// `CoVariant` and the program compiles, while stopping at the enclosing class
// gives `Invariant` and rejects it. So this row is the one that makes
// `walkNestedTypePath` load-bearing rather than decorative.

BEZEL_TEST(head_qualifier_6373, nested_covariant_imported_head_accepted) {
    auto r = build6373(
        lib6373("public class Outer "
                "  public interface Box<out T> method Get(): T; end Box; "
                "end Outer; "),
        app6373("import Lib; ",
                "public interface I<out T> "
                "  method Get(): Lib.Outer.Box<T>; "
                "end I; "));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
}

// --- 2. a MULTI-SEGMENT module name is reachable --------------------------
//
// §2.2 admits `import Com.Sub;` — an import names a module by its DOTTED name,
// and `sourceImports_` is keyed by that whole string. So for the spelling
// `Com.Sub.Box<T>` the module consumes the first TWO segments and `Box` is the
// type. A `parts.front()`-only test looks up `Com`, which is not an imported
// module name, resolves nothing, and leaves a real §12.9 violation silently
// accepted: `Com.Sub`'s `Box` is `in T`, and `out T` in a return slot composed
// through it is contravariant.

BEZEL_TEST(head_qualifier_6373, dotted_module_name_head_rejected) {
    auto r = build6373(
        "module Com.Sub; " + contraHead("Box"),
        app6373("import Com.Sub; ",
                "public interface I<out T> "
                "  method Get(): Com.Sub.Box<T>; "
                "end I; "));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(r.err.find("type parameter `T` is covariant (`out`)") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.err.find(kVariance129) != std::string::npos);
    BEZEL_EXPECT_TRUE(r.err.find(kComposedClause) != std::string::npos);
}

// The accept side of the same spelling, so the row above cannot be satisfied by
// refusing to resolve dotted module names at all.

BEZEL_TEST(head_qualifier_6373, dotted_module_name_covariant_head_accepted) {
    auto r = build6373(
        "module Com.Sub2; " + coHead("Box"),
        app6373("import Com.Sub2; ",
                "public interface I<out T> "
                "  method Get(): Com.Sub2.Box<T>; "
                "end I; "));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
}

// --- 3. a LOCAL ALIAS shadows a module name (§2.2) -------------------------
//
// The shadowing guard consulted `localTypes`, which holds `TypeSymbol`s, and
// never `localAliases` — so a local alias sharing a module's name did not stop
// the import arm. Here the alias is what the name resolver binds: `Lib.Box<T>`
// resolves `Lib` to the ALIAS in module scope, `Lib` the alias names nothing
// with a member `Box`, and §27.8's arity rule rejects the type argument. §12.9
// must not also report a variance read off the MODULE's `Box<in T>` — a type
// this spelling never reaches.

BEZEL_TEST(head_qualifier_6373, local_alias_shadowing_module_name_skips_variance) {
    auto r = build6373(lib6373(contraHead("Box")),
                       app6373("import Lib; ",
                               "type Lib := Array<int>; "
                               "public interface I<out T> "
                               "  method Get(): Lib.Box<T>; "
                               "end I; "));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(r.err.find("type alias `Lib`") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.err.find(kVariance129) == std::string::npos);
}

// --- 4. a QUALIFIED spelling is not answered off the LOCAL top-level table --
//
// The local-type walk compared `parts.back()` with no qualifier test, so
// `Lib.Box<T>` was answered off a LOCAL top-level `Box` whenever the file
// declared one. §2.7 rule 2 binds `Lib.Box` in the MODULE: a local declaration
// shadows an import under §2.2, and §2.2's rule is about a BARE name.
//
// Configuration A — local `Box<in T>`, `Lib`'s `Box<out T>`. The module's head
// is covariant, so at a return slot `out T` is honoured and the program must
// COMPILE. Answering off the local head reads `in T`, which is a false resolve
// that turns a legal program into an error — the exact shape #6351's first
// attempt was rejected for.

BEZEL_TEST(head_qualifier_6373,
           qualified_spelling_reads_imported_head_not_local) {
    auto r = build6373(lib6373(coHead("Box")),
                       app6373("import Lib; ",
                               "public interface Box<in T> "
                               "  method Put(x: T); "
                               "end Box; "
                               "public interface I<out T> "
                               "  method Get(): Lib.Box<T>; "
                               "end I; "));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
}

// Configuration B — the mirror, local `Box<out T>`, `Lib`'s `Box<in T>`. Here
// the correct answer is a §12.9 REJECTION, and answering off the local head
// skips it: a real variance hole, accepted silently.

BEZEL_TEST(head_qualifier_6373,
           qualified_spelling_missed_contravariant_import_rejected) {
    auto r = build6373(lib6373(contraHead("Box")),
                       app6373("import Lib; ",
                               "public interface Box<out T> "
                               "  method Get(): T; "
                               "end Box; "
                               "public interface I<out T> "
                               "  method Get(): Lib.Box<T>; "
                               "end I; "));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(r.err.find("type parameter `T` is covariant (`out`)") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.err.find(kVariance129) != std::string::npos);
}

// The CONTROL for both: the SAME file and the SAME two types as
// `qualified_spelling_reads_imported_head_not_local`, spelled BARE. §2.2 — a
// local declaration shadows an import — makes the bare `Box<T>` the LOCAL
// `in T`, so §12.9 must still reject it. If the qualifier test had leaked into
// the bare-name path, this row would go green and the module's contribution to
// §2.2's shadowing would be gone with it.

BEZEL_TEST(head_qualifier_6373, bare_spelling_still_reads_local_head) {
    auto r = build6373(lib6373(coHead("Box")),
                       app6373("import Lib; ",
                               "public interface Box<in T> "
                               "  method Put(x: T); "
                               "end Box; "
                               "public interface I<out T> "
                               "  method Get(): Box<T>; "
                               "end I; "));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(r.err.find(kVariance129) != std::string::npos);
}

// --- 5. the built-in table answers bare names only (§2.3) ------------------
//
// `LibX` exports no `Array`. The spelling `LibX.Array<T>` therefore names
// nothing — §2.7 rule 2 resolves `LibX` to the module and finds no `Array` —
// yet the built-in table, reached on `parts.back()` like every other arm, handed
// back the built-in `Array` and produced a §12.9 rejection byte-identical to
// the one the bare `Array<T>` spelling legitimately earns (the control below).

BEZEL_TEST(head_qualifier_6373, qualified_builtin_spelling_skips_variance) {
    auto r = build6373(
        "module LibX; public interface Other<out T> method Get(): T; "
        "end Other; ",
        app6373("import LibX; ",
                "public interface I<out T> "
                "  method Put(x: LibX.Array<T>); "
                "end I; "));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(r.err.find("module `LibX` has no public type `Array`") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.err.find(kVariance129) == std::string::npos);
}

// Control: the BARE spelling of the same built-in in the same position is a
// real §12.9 violation, and `Array` is invariant so `out T` cannot honour an
// input position. No library.

BEZEL_TEST(head_qualifier_6373, bare_builtin_spelling_still_rejected) {
    auto r = build6373(
        "",
        "module AppBare; import System; "
        "public interface I<out T> method Put(x: Array<T>); end I; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(\"x\"); return 0; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(r.err.find(kVariance129) != std::string::npos);
}

// Control: the built-in reached by its HOME MODULE's qualified name, which
// §2.3 admits ("Use without import; naming requires import" is satisfied by the
// auto-import, and `resolveNamed` rewrites the qualifier away in the AST before
// the variance pass reads it). The rejection is real and must survive — this is
// the row that says the qualifier gate did not swallow a legal qualified
// built-in spelling.

BEZEL_TEST(head_qualifier_6373, home_module_qualified_builtin_still_rejected) {
    auto r = build6373(
        "",
        "module AppHome; import System; "
        "public interface I<out T> "
        "  method Put(x: System.Collections.Generic.Array<T>); "
        "end I; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(\"x\"); return 0; "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(r.err.find(kVariance129) != std::string::npos);
}

}  // namespace
}  // namespace bezel::module