// Regression test for #3575 — module-imported §11.6 nested type as a BASE
// CLAUSE must bind even when the deriving type is declared BEFORE its base's
// enclosing type in the exporting module.
//
// AUTHORITY.
//
// spec §11.6 (v0.286), "Nested types", verbatim:
//
//     "A nested type is referenced from outside as `Outer.Nested`."
//
// spec §2.8.3 rule 3 (v0.286), "Program-wide type identity", verbatim:
//
//     "A type is identified program-wide by its **declaring module name, its
//      qualified type name (including any enclosing-type path, [§11.6]), and
//      its generic arity**"
//
// spec §12.1 / §11.3 (v0.286): a base clause resolves to its named type
// regardless of declaration order within the source module; the importing
// side inherits the binding.
//
// THE DEFECT (per the issue body). On the #3540 tree, a top-level
// `class Derived: Outer.Base` whose declaring `Outer` (and `Outer.Base`) is
// declared LATER in the same source module failed to bind at the loader:
// `loadMetadataPass2` visited `Derived` first, the per-type base-vector guard
// `if (ref->kind == Named && ref->declaration)` declined the unresolved
// `Outer.Base` (its shell didn't exist yet), and `#3540`'s
// `resolvePendingNamed` retry ran AFTER the guard had passed — so
// `Derived.baseTypes` stayed empty and `lookupMember` could not walk up to
// `Outer.Base`'s members. The consumer then rejected `d.BaseOnly()` with
// "no member `BaseOnly` on type `Derived`" (§11.3 / §12.1).
//
// On the #3581 tree (HEAD at HEAD = c893895), `registerNestedTypeShells` is
// called from `loadMetadataPass1` and writes every nested-type shell into
// `localTypes` (and the cross-module `importedByModule_` table) BEFORE
// pass 2 starts. Pass 2's `lookupNamed("Outer.Base")` therefore succeeds
// during the per-type base-vector conversion of `Derived`, the guard holds,
// and `baseTypes` is populated in DECLARED order — fixing #3575 as a
// side-effect of the same pass-1 shell mechanism #3581 introduced for the
// signature case. This test pins the resulting behaviour so a future
// regression of #3581's shell mechanism is caught.
//
// REPRO. Same shape as the issue's "Measured repro (on the fix #3540 tree)":
//
//   libz.bzl:
//     public class Derived: Outer.Base      // declared FIRST
//       public constructor(v: int) base(v); end constructor;
//     end Derived;
//     public class Outer
//       public class Base
//         protected _v: int;
//         public constructor(v: int) _v := v; end constructor;
//         public method BaseOnly(): int => _v + 100;
//       end Base;
//     end Outer;
//
//   appz.bzl:
//     import LibZ;
//     using var d := new Derived(6);
//     Console.WriteLine($"inh={d.BaseOnly()}");
//
// On the broken tree this rejects with "no member `BaseOnly` on type
// `Derived`". On the fixed tree this compiles, links, runs and prints
// "inh=106" — proving the base clause binds (the inherited `BaseOnly()`
// returns `6 + 100 = 106`, the value `_v` was set via the base ctor chain
// the `base(v)` forwarded to).
//
// ROWS.
//
//   * T1 — the headline repro: Derived BEFORE Outer, with `Outer.Base` as
//     the named base. The row the fix changes. Pre-#3581 the consumer
//     rejected with "no member `BaseOnly`"; post-#3581 the consumer
//     compiles, links, runs, and prints `inh=106`.
//
//   * T2 — Derived BEFORE Outer, with a SECOND inheritance base shape
//     (class + interface). Pins the issue's "the order is load-bearing"
//     concern: a class base must precede interfaces for the layout prefix,
//     and a retry-time append would flip that order. The combined fix
//     must preserve declared order; this row pins it for the top-level
//     pass.
//
//   * T3 — NESTED-SIBLING variant (issue's "reconstructTypeSymbol has a
//     sibling base loop"): inside a single enclosing type, a nested type
//     `Derived` extends a sibling `Base` declared LATER in the same
//     `Outer`. Pins that the pass-1 recursive shell walker also covers
//     this path (a fix that only patched the top-level pass would not).
//
//   * T4 — Outer-first control: declaring `Outer` before `Derived` was
//     already working pre-#3581; this row pins the fix did not widen
//     past its subject.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3575 {
    std::string path;
    explicit TempFile3575(std::string p) : path(std::move(p)) {}
    ~TempFile3575() { std::remove(path.c_str()); }
};

void writeSource3575(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `libSrc` to an object, then compile `appSrc` against it, run the
// resulting executable in a separate process, and return the captured
// stdout text + exit code. Mirrors the simpler harness pattern of #2449 /
// #2387 / #2414 (one library + one consumer), since #3575's repro is itself
// one library + one consumer.
//
// The scratch dir comes from `wtScratchDir` (support/scratch_dir.hpp) —
// CLAUDE.md forbids fs::temp_directory_path() because the box's /tmp is
// tmpfs (RAM) and the suite's AOT scratch must NOT live there.
struct RunOut3575 {
    int status = -1;          // -1: build/run failure; -2: signal death
    std::string out;          // captured stdout
};

// Helper: compile `libSrc` to `libObj`; returns true on success.
bool compileLib3575(const std::string& libSrc, const std::string& libObj) {
    bezel::Driver driver;
    return driver.compileObject(libSrc, libObj).success;
}

// Helper: compile `appSrc` to `appExe` against `libObj` (single dep); run it;
// capture stdout; report via `RunOut3575`. -1 on compile/link failure,
// -2 if the dep itself could not be produced (caller checks before).
RunOut3575 compileImportAndRun3575(const std::string& libObj,
                                   const std::string& appSrc,
                                   const std::string& tag) {
    RunOut3575 r;
    bezel::Driver driver;
    auto dir = wtScratchDir("wt-3575");
    std::string stem = unique_stem(("bz3575_" + tag + "_").c_str());
    TempFile3575 appSrcFile((dir / (stem + "_app.bzl")).string());
    TempFile3575 appExe((dir / stem).string());
    TempFile3575 appOut((dir / (stem + ".out")).string());
    writeSource3575(appSrcFile.path, appSrc);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObj;
    d.objectPath = libObj;
    deps.push_back(d);
    if (!driver.compileFile(appSrcFile.path, appExe.path, deps).success)
        return r;
    std::string cmd = appExe.path + " > " + appOut.path + " 2>&1";
    int status = std::system(cmd.c_str());
    {
        std::ifstream in(appOut.path);
        std::ostringstream ss;
        ss << in.rdbuf();
        r.out = ss.str();
    }
    if (status == -1) return r;
    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : -2;
    return r;
}

// Run the full repro from a library source + a consumer source: produce the
// library object, then compile + link + run the consumer.
RunOut3575 reproBuild(const std::string& libSrc, const std::string& appSrc,
                      const std::string& tag) {
    auto dir = wtScratchDir("wt-3575");
    std::string stem = unique_stem(("bz3575_" + tag + "_lib_").c_str());
    TempFile3575 libSrcFile((dir / (stem + ".bzl")).string());
    TempFile3575 libObjFile((dir / (stem + ".o")).string());
    writeSource3575(libSrcFile.path, libSrc);
    if (!compileLib3575(libSrcFile.path, libObjFile.path)) {
        RunOut3575 r;
        r.status = -2;
        return r;
    }
    return compileImportAndRun3575(libObjFile.path, appSrc, tag);
}

// -- The defect --------------------------------------------------------------

// T1 — spec §11.6 / §2.8.3 rule 3 / §12.1, v0.286, #3575. THE ROW the
// pass-1 shell mechanism (c7-l2 fix #3581) is being pinned against. Library
// declares `Derived` FIRST, then `Outer` with a nested `Base`. The consumer
// imports the library and calls the inherited `d.BaseOnly()` method. On the
// pre-#3581 tree this REJECTED with "no member `BaseOnly` on type `Derived`
// (§11.3, §12.1)" — `Derived.baseTypes` was empty because the base-clause
// guard declined the still-unresolved `Outer.Base` reference, and
// `resolvePendingNamed` ran after the guard had passed. On the #3581 tree
// (HEAD) `Outer.Base`'s shell is registered in `localTypes` during pass 1,
// the guard holds, the base is recorded in `baseTypes` in DECLARED order,
// and the consumer compiles, links, runs and prints `inh=106`.
BEZEL_TEST(xmod_nested_type_as_base_clause_3575,
           derived_before_outer_binds_inherited_member) {
    constexpr const char* kLib =
        "module LibZ; "
        "public class Derived: Outer.Base "
        "  public constructor(v: int) base(v); end constructor; "
        "end Derived; "
        "public class Outer "
        "  public class Base "
        "    protected _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method BaseOnly(): int => _v + 100; "
        "  end Base; "
        "end Outer; ";
    constexpr const char* kApp =
        "module AppZ; "
        "import LibZ; "
        "class Program "
        "  public static method Main(): int "
        "    using var d := new Derived(6); "
        "    Console.WriteLine($\"inh={d.BaseOnly()}\"); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    RunOut3575 r = reproBuild(kLib, kApp, "t1");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("inh=106\n"));
}

// T2 — same order (Derived BEFORE Outer) with TWO bases, class + interface,
// pinning that declared order is preserved by the fix. Pre-#3581 a retry-
// time append would have flipped the order (class must precede interface
// for the layout prefix — the issue's "Why #3540 did not just fix it here"
// paragraph records exactly this load-bearing detail). On the fixed tree the
// consumer builds and answers `inh=106 foo=42`.
BEZEL_TEST(xmod_nested_type_as_base_clause_3575,
           derived_before_outer_with_class_and_interface_preserves_order) {
    constexpr const char* kLib =
        "module LibZ; "
        "public interface IFoo "
        "  method Foo(): int; "
        "end IFoo; "
        "public class Derived: Outer.Base, IFoo "
        "  public constructor(v: int) base(v); end constructor; "
        "  public method Foo(): int => 42; "
        "end Derived; "
        "public class Outer "
        "  public class Base "
        "    protected _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method BaseOnly(): int => _v + 100; "
        "  end Base; "
        "end Outer; ";
    constexpr const char* kApp =
        "module AppZ; "
        "import LibZ; "
        "class Program "
        "  public static method Main(): int "
        "    using var d := new Derived(6); "
        "    Console.WriteLine($\"inh={d.BaseOnly()} foo={d.Foo()}\"); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    RunOut3575 r = reproBuild(kLib, kApp, "t2");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("inh=106 foo=42\n"));
}

// T3 — issue's "reconstructTypeSymbol has a sibling base loop" concern:
// inside a single enclosing type, a nested `Derived` extends a sibling
// `Base` declared LATER in the same `Outer`. Both the top-level
// `loadMetadataPass2` walk AND `reconstructTypeSymbol`'s own base loop
// must defer the per-type base push past `resolvePendingNamed`. Pinning
// the nested-sibling case keeps a fix that only patched the top-level
// pass from claiming to close the issue.
//
// (Subtle: the path here is `Outer.Derived: Outer.Base` — `Base` is a
// sibling of `Derived` inside the same `Outer`. The shell mechanism must
// pre-register `Outer.Base` BEFORE pass 2's `reconstructTypeSymbol(Derived)`
// runs the base loop, which `registerNestedTypeShells` does recursively.)
BEZEL_TEST(xmod_nested_type_as_base_clause_3575,
           nested_sibling_derived_before_base_binds_inherited_member) {
    constexpr const char* kLib =
        "module LibZ; "
        "public class Outer "
        "  public class Derived: Outer.Base "
        "    public constructor(v: int) base(v); end constructor; "
        "  end Derived; "
        "  public class Base "
        "    protected _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method BaseOnly(): int => _v + 100; "
        "  end Base; "
        "end Outer; ";
    constexpr const char* kApp =
        "module AppZ; "
        "import LibZ; "
        "class Program "
        "  public static method Main(): int "
        "    using var d := new Outer.Derived(6); "
        "    Console.WriteLine($\"inh={d.BaseOnly()}\"); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    RunOut3575 r = reproBuild(kLib, kApp, "t3");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("inh=106\n"));
}

// T4 — control (spec §11.6 / §2.8.3 rule 3, v0.286). Declaring `Outer`
// before `Derived` was the shape #3540 already handled (the base's shell
// existed by the time `Derived`'s base vector was converted). This row
// pins that the fix did NOT widen past the subject — `Outer`-first was
// always passing, and must keep passing.
BEZEL_TEST(xmod_nested_type_as_base_clause_3575,
           outer_first_control_still_binds) {
    constexpr const char* kLib =
        "module LibZ; "
        "public class Outer "
        "  public class Base "
        "    protected _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method BaseOnly(): int => _v + 100; "
        "  end Base; "
        "end Outer; "
        "public class Derived: Outer.Base "
        "  public constructor(v: int) base(v); end constructor; "
        "end Derived; ";
    constexpr const char* kApp =
        "module AppZ; "
        "import LibZ; "
        "class Program "
        "  public static method Main(): int "
        "    using var d := new Derived(6); "
        "    Console.WriteLine($\"inh={d.BaseOnly()}\"); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    RunOut3575 r = reproBuild(kLib, kApp, "t4");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("inh=106\n"));
}

}  // namespace
}  // namespace bezel::module