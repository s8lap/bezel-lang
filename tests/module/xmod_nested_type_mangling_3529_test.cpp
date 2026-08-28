// Cross-module regression tests for fix #3529 — the ENCLOSING-TYPE half of spec
// §2.8.3 rule 3's program-wide type identity, at the artifact boundary.
//
// AUTHORITY. spec §2.8.3 rule 3 (v0.283), "Program-wide type identity",
// verbatim:
//
//     "A type is identified program-wide by its **declaring module name, its
//      qualified type name (including any enclosing-type path, [§11.6]), and
//      its generic arity**"
//
// and, on why the two sides must agree, the same rule:
//
//     "All descriptions of one type — the declaring artifact's own and every
//      copy embedded in dependents' interfaces ([§2.8.2](#sec-artifact-interface)
//      clause 3) — denote the same type and **must agree**"
//
// WHY A CROSS-MODULE ROW IS NOT OPTIONAL FOR THIS FIX. A mangling change is an
// artifact-interface change: the exporter DEFINES `bz$<id>$ctor` and every
// importer emits an external DECLARATION of the same name. If the two sides
// computed the enclosing-type path differently the result would be a link error
// at best and a wild call at worst, which is strictly worse than the collision
// being fixed. That is why `TypeSymbol::enclosingTypePath` is populated at
// symbol-creation time on BOTH sides — `SymbolCollection::collectTypeDecl` for a
// local declaration, `ModuleLoader::reconstructTypeSymbol` for an imported one —
// exactly as #1488 populates `moduleName`, rather than being re-derived in
// codegen from a structure only one side has.
//
// THE DEFECT, MEASURED on b43196a. A library declaring `Outer1.Box` and
// `Outer2.Box` emits `bz$LibN.Box$ctor` for the first and an LLVM-uniquified
// `bz$LibN.Box$ctor.N` for the second, where N is that MODULE's numbering. A
// consumer constructing both picks its OWN N and the link fails outright:
//
//     /usr/bin/ld: …xappn.o: in function `main':
//     bezel:(.text+0x303): undefined reference to `bz$LibN.Box$ctor.6'
//
// — "a symbol that exists nowhere", verbatim what #1488's own comment in
// LlvmBackend_impl.hpp predicted for exactly this shape. Post-fix the two names
// are `bz$LibN.Outer1.Box$ctor` and `bz$LibN.Outer2.Box$ctor`, and the program
// runs.
//
// The single-module rows (wrong comparator, SIGSEGV on differing layouts) live
// in tests/codegen/nested_type_mangling_collision_3529_test.cpp.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

// Per-process scratch dir under the shared base, cleaned up at exit — never
// fs::temp_directory_path(), which is the tmpfs CLAUDE.md forbids for scratch.
const fs::path& wtDir3529() { return wtScratchDir("wt-3529"); }

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// status: the program's exit code, -1 when a compile OR LINK failed, -2 when the
// program died on a signal. The three are kept distinct on purpose: the pre-fix
// defect here is a LINK failure, which must not read as "ran and answered 0".
struct RunOut {
    int status = -1;
    std::string out;
};

// Compile N library modules to objects, compile a consumer that imports them,
// link, run, and return the status and stdout. Mirrors buildAndRunTwoLibs in
// xmod_class_comparator_collision_3512_test.cpp (which is in that file's
// anonymous namespace and so not reusable) generalized to one-or-two libraries.
RunOut buildAndRunLibs(const std::vector<std::string>& libSrcs,
                       const std::string& mainSrc, const std::string& tag) {
    RunOut r;
    auto dir = wtDir3529();
    std::string stem = "bz3529_" + tag + "_" + unique_stem("");

    std::vector<std::unique_ptr<TempFile>> keep;
    std::vector<std::string> objs;
    bezel::Driver driver;
    for (std::size_t i = 0; i < libSrcs.size(); ++i) {
        auto src = std::make_unique<TempFile>(
            (dir / (stem + "_lib" + std::to_string(i) + ".bzl")).string());
        auto obj = std::make_unique<TempFile>(
            (dir / (stem + "_lib" + std::to_string(i) + ".o")).string());
        writeSource(src->path, libSrcs[i]);
        if (!driver.compileObject(src->path, obj->path).success) return r;
        objs.push_back(obj->path);
        keep.push_back(std::move(src));
        keep.push_back(std::move(obj));
    }

    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / stem).string());
    TempFile mOut((dir / (stem + ".out")).string());
    writeSource(mSrc.path, mainSrc);

    std::vector<bezel::Dependency> deps;
    for (const std::string& obj : objs) {
        bezel::Dependency d;
        d.metadataPath = obj;
        d.objectPath = obj;
        deps.push_back(d);
    }
    if (!bezel::test::compileFileForTest(driver, mSrc.path, mExe.path, deps)
             .success)
        return r;

    std::string cmd = mExe.path + " > " + mOut.path + " 2>&1";
    int status = std::system(cmd.c_str());
    {
        std::ifstream in(mOut.path);
        std::ostringstream ss;
        ss << in.rdbuf();
        r.out = ss.str();
    }
    if (status == -1) return r;
    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : -2;
    return r;
}

// One library, two same-named nested classes with DIFFERENT field layouts (one
// `int` against two `int`s), so a consumer that reached the wrong `$ctor` would
// not merely mis-answer — it would run the wrong initializer over the wrong
// object size. `Util.Sum` uses both INSIDE the library, which is the exporter
// half of the agreement.
const char* kLibNested =
    "module LibN; "
    "public class Outer1 "
    "  public class Box "
    "    private _v: int; "
    "    public constructor(v: int) _v := v; end constructor; "
    "    public method Value(): int => _v; "
    "  end Box; "
    "end Outer1; "
    "public class Outer2 "
    "  public class Box "
    "    private _a: int; "
    "    private _b: int; "
    "    public constructor(a: int, b: int) _a := a; _b := b; end constructor; "
    "    public method Value(): int => _a * 100 + _b; "
    "  end Box; "
    "end Outer2; "
    "public class Util "
    "  public static method Sum(): int "
    "    using var x := new Outer1.Box(7); "
    "    using var y := new Outer2.Box(3, 4); "
    "    return x.Value() + y.Value(); "
    "  end Sum; "
    "end Util; ";

}  // namespace

// ── The defect ─────────────────────────────────────────────────────────────

// per spec §2.8.3 rule 3 + §11.6, v0.283, #3529: a consumer that constructs BOTH
// of an imported module's same-named nested classes must link and answer. Pre-fix
// both mangled to "LibN.Box", the exporter emitted one `$ctor` plus one
// LLVM-uniquified `$ctor.N`, and the consumer's own numbering produced
// `undefined reference to bz$LibN.Box$ctor.6` — MEASURED on b43196a, with the
// two-int type's constructor the one that vanished.
//
// The status is asserted alongside the output because the pre-fix failure is a
// LINK failure: the helper reports status -1 and an EMPTY stdout for it, which an
// output-only assertion would report as an ordinary wrong answer.
BEZEL_TEST(xmod_nested_type_mangling_3529,
           consumer_reaches_both_imported_same_named_nested_types) {
    RunOut r = buildAndRunLibs({kLibNested},
        "module App; import LibN; "
        "class Program public static method Main(): int "
        "  using var p := new Outer1.Box(7); "
        "  using var q := new Outer2.Box(3, 4); "
        "  Console.WriteLine($\"p={p.Value()} q={q.Value()}\"); "
        "  return 0; "
        "end Main; end Program; ", "twonested");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("p=7 q=304\n"));
}

// ── Controls: these pass on BOTH trees ─────────────────────────────────────

// CONTROL (spec §2.8.3 rule 3, v0.283, #1488) — two SEPARATE libraries each
// declaring a nested `Outer.Box` were ALREADY distinguished by the module half
// of rule 3 ("LibA.Box" / "LibB.Box"). This row is what proves adding the
// enclosing-type half did not disturb the module half: it must keep passing, and
// the two `Box`es keep their own differing layouts and their own answers.
BEZEL_TEST(xmod_nested_type_mangling_3529,
           two_libraries_nested_same_name_still_distinguished_control) {
    RunOut r = buildAndRunLibs(
        {"module LibA; "
         "public class Outer "
         "  public class Box "
         "    private _v: int; "
         "    public constructor(v: int) _v := v; end constructor; "
         "    public method Value(): int => _v; "
         "  end Box; "
         "end Outer; ",
         "module LibB; "
         "public class Outer "
         "  public class Box "
         "    private _a: int; "
         "    private _b: int; "
         "    public constructor(a: int, b: int) _a := a; _b := b; "
         "    end constructor; "
         "    public method Value(): int => _a * 100 + _b; "
         "  end Box; "
         "end Outer; "},
        "module App; import LibA; import LibB as B; "
        "class Program public static method Main(): int "
        "  using var p := new LibA.Outer.Box(7); "
        "  using var q := new B.Outer.Box(3, 4); "
        "  Console.WriteLine($\"p={p.Value()} q={q.Value()}\"); "
        "  return 0; "
        "end Main; end Program; ", "twolibs");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("p=7 q=304\n"));
}

// CONTROL (spec §2.8.3 rule 3 + §11.6, v0.283) — the AGREEMENT row, in both
// directions at once. The consumer declares its OWN `Outer1.Box` whose simple
// name AND enclosing-type path match an imported one, uses it alongside the
// imported type, and also calls a library method that constructs both of the
// LIBRARY's nested types internally. That exercises: the exporter's own
// definitions (`Util.Sum`), the importer's external declarations (`theirs`), and
// a purely local nested type in the same program (`mine`). All three must
// coexist — "App.Outer1.Box" and "LibN.Outer1.Box" are distinct because the
// MODULE component still differs, which is why this row passes on both trees;
// it is here to catch a fix that qualified by enclosing type while LOSING the
// module component.
BEZEL_TEST(xmod_nested_type_mangling_3529,
           local_and_imported_nested_same_name_coexist_control) {
    RunOut r = buildAndRunLibs({kLibNested},
        "module App; import LibN; "
        "class Outer1 "
        "  public class Box "
        "    private _s: string; "
        "    public constructor(s: string) _s := s; end constructor; "
        "    public method Label(): string => _s; "
        "  end Box; "
        "end Outer1; "
        "class Program public static method Main(): int "
        "  using var mine := new Outer1.Box(\"local\"); "
        "  using var theirs := new LibN.Outer1.Box(7); "
        "  Console.WriteLine($\"mine={mine.Label()} theirs={theirs.Value()} "
        "sum={LibN.Util.Sum()}\"); "
        "  return 0; "
        "end Main; end Program; ", "mixed");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("mine=local theirs=7 sum=311\n"));
}


// CONTROL — and the reason the mangling's nesting separator is `+` rather than
// `.`. spec §2.8.3 rule 3's identity is a TUPLE — "its declaring module name,
// its qualified type name (including any enclosing-type path, [§11.6]), and its
// generic arity" — but a mangled symbol is one flat string, so the separator has
// to keep the tuple boundary recoverable. A module name is itself dotted, so a
// DOTTED nesting join makes these two libraries render the identical symbol:
//
//     module M.Outer1;  public class Ord              ->  "M.Outer1.Ord"
//     module M;  public class Outer1 { class Ord }    ->  "M.Outer1.Ord"
//
// MEASURED. This exact program links, runs, and prints "x=7" on b43196a. Against
// an intermediate build of THIS fix that joined the nesting path with `.`, the
// same program failed with six link errors —
//
//     multiple definition of `bz$M.Outer1.Ord$ctor'
//     multiple definition of `bz$M.Outer1.Ord$Dispose'
//     multiple definition of `bz$M.Outer1.Ord$DisposeCore'
//     multiple definition of `bz$M.Outer1.Ord$init'
//     multiple definition of `vt$M.Outer1.Ord'
//     multiple definition of `td$M.Outer1.Ord'
//
// — a regression the dotted spelling INTRODUCES, since pre-fix the nested type
// was "M.Ord" and could not collide with anything module-qualified. With the
// shipped `+` join the two render "M.Outer1.Ord" and "M.Outer1+Ord" and it links
// again (measured: "x=7" on the fix tree too). `+` cannot appear in a Bezel
// identifier or in a dotted module name, so it can only ever mean "the
// enclosing-type path starts here".
//
// This row therefore passes on BOTH trees by construction — it exists to fail on
// the tempting, wrong version of the fix.
BEZEL_TEST(xmod_nested_type_mangling_3529,
           module_path_and_nesting_path_do_not_collide_control) {
    RunOut r = buildAndRunLibs(
        {"module M.Outer1; "
         "public class Ord "
         "  private _v: int; "
         "  public constructor(v: int) _v := v; end constructor; "
         "  public method Value(): int => _v; "
         "end Ord; ",
         "module M; "
         "public class Outer1 "
         "  public class Ord "
         "    private _a: int; "
         "    private _b: int; "
         "    public constructor(a: int, b: int) _a := a; _b := b; "
         "    end constructor; "
         "    public method Value(): int => _a * 100 + _b; "
         "  end Ord; "
         "end Outer1; "},
        "module App; import M.Outer1; "
        "class Program public static method Main(): int "
        "  using var x := new Ord(7); "
        "  Console.WriteLine($\"x={x.Value()}\"); "
        "  return 0; "
        "end Main; end Program; ", "ambiguity");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("x=7\n"));
}

}  // namespace bezel::module
