// Cross-module regression tests for fix #3540 — an imported §11.6 NESTED type
// named in a member signature must resolve to that type, not to the i32
// fallback.
//
// AUTHORITY.
//
// spec §11.6 (v0.285), "Nested types", verbatim:
//
//     "A nested type is referenced from outside as `Outer.Nested`."
//
// spec §2.8.3 rule 3 (v0.285), "Program-wide type identity", verbatim:
//
//     "A type is identified program-wide by its **declaring module name, its
//      qualified type name (including any enclosing-type path, [§11.6]), and
//      its generic arity**"
//
// and, on the two sides of an artifact boundary:
//
//     "All descriptions of one type — the declaring artifact's own and every
//      copy embedded in dependents' interfaces ([§2.8.2] clause 3) — denote the
//      same type and **must agree**"
//
// THE DEFECT, MEASURED on fca2f02 with a real two-artifact build. `MetaType`'s
// own header says `namedName` "carries the referent's fully-qualified name by
// value", but `ModuleMetadataBuilder::convertType` wrote
// `ref->declaration->name` — the SIMPLE identifier — so a nested type crossed
// the boundary as "Box". On the importing side nothing had ever entered a
// nested type into `localTypes`: `loadMetadataPass1` walks `md.types`, which is
// the module's TOP-LEVEL types, and a nested type's symbol does not exist until
// pass 2 reaches its enclosing type's `MetaMemberKind::NestedType` member. So
// `lookupNamed` returned null, the parameter's TypeRef kept
// `declaration == nullptr`, and codegen lowered it to the i32 default (#495's
// fallback):
//
//     Call parameter type does not match function signature!
//       %29 = load ptr, ptr %5, align 8
//      i32  %30 = call i32 @"bz$LibS.Outer+Box$Plus"(ptr %28, ptr %29)
//     bezelc: codegen error: IR verification failed
//
// A `ptr` passed to an `i32` parameter. The consumer could not be compiled at
// all, and the failure named an IR invariant rather than any language rule —
// the worst shape of a resolution gap, because it is not a rejection, it is a
// pointer truncated to 32 bits that only the verifier happened to catch.
//
// WHY THE KEY MUST BE THE QUALIFIED NAME. #3529 is the record of `Outer1.Ord`
// and `Outer2.Ord` being two unrelated types under rule 3 above. Keying the
// importer's table on the simple name would make one of them answer for the
// other — the same collision #3529 removed from the linker, reintroduced in the
// importer's type table, where the two types' differing field layouts make it
// unsound rather than merely wrong. `same_simple_name_under_two_enclosing_types`
// is the row that pins it: its two `Box`es hold an `int` and a `string`.
//
// EVERY ROW IS A GENUINE TWO-ARTIFACT BUILD — `Driver::compileObject` for each
// library, then a consumer compiled against it with `--import`, linked and RUN
// — because this defect lives entirely in what one artifact writes and another
// reads. A single-module program cannot see it: the exporter's own use of
// `Outer.Box` resolves through sema, never through the metadata.

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
const fs::path& wtDir3540() { return wtScratchDir("wt-3540"); }

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// status: the program's exit code, -1 when a compile OR LINK failed, -2 when
// the program died on a signal. The three are kept distinct on purpose: the
// pre-fix defect here is a CONSUMER COMPILE failure, which must not read as
// "ran and answered 0". Every row therefore asserts the status as well as the
// output — an output-only assertion would compare "" against the expected text
// and report an ordinary wrong answer, hiding that nothing ran at all.
struct RunOut {
    int status = -1;
    std::string out;
};

// Compile one library module to an object, compile a consumer that imports it,
// link, run, and return the status and stdout. Same shape as
// buildAndRunLibs in xmod_nested_type_mangling_3529_test.cpp (which is in that
// file's anonymous namespace and so not reusable), narrowed to one library
// because every shape here is about ONE artifact's own nested types.
RunOut buildAndRun(const std::string& libSrc, const std::string& mainSrc,
                   const std::string& tag) {
    RunOut r;
    auto dir = wtDir3540();
    std::string stem = "bz3540_" + tag + "_" + unique_stem("");

    bezel::Driver driver;
    TempFile lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile lObj((dir / (stem + "_lib.o")).string());
    writeSource(lSrc.path, libSrc);
    if (!driver.compileObject(lSrc.path, lObj.path).success) return r;

    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / stem).string());
    TempFile mOut((dir / (stem + ".out")).string());
    writeSource(mSrc.path, mainSrc);

    bezel::Dependency d;
    d.metadataPath = lObj.path;
    d.objectPath = lObj.path;
    std::vector<bezel::Dependency> deps{d};
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

}  // namespace

// -- The defect ------------------------------------------------------------

// per spec §11.6 + §2.8.3 rule 3, v0.285, #3540 — the issue's own repro: a
// method on a nested class whose parameter is that same nested type. Pre-fix
// the consumer failed IR verification with `call i32 @"bz$LibS.Outer+Box$Plus"
// (ptr, ptr)`; post-fix it links and answers 7 + 5.
BEZEL_TEST(xmod_nested_type_in_signature_3540,
           nested_type_as_parameter_of_its_own_method) {
    RunOut r = buildAndRun(
        "module LibS; "
        "public class Outer "
        "  public class Box "
        "    private _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method Value(): int => _v; "
        "    public method Plus(other: Outer.Box): int => _v + other.Value(); "
        "  end Box; "
        "end Outer; ",
        "module App; import LibS; "
        "class Program public static method Main(): int "
        "  using var a := new Outer.Box(7); "
        "  using var b := new Outer.Box(5); "
        "  Console.WriteLine($\"sum={a.Plus(b)}\"); "
        "  return 0; "
        "end Main; end Program; ", "self");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("sum=12\n"));
}

// per spec §11.6, v0.285, #3540 — a SIBLING nested type declared LATER in the
// same enclosing type. This row is what the eager registration in
// `reconstructTypeSymbol` alone cannot satisfy: when `Outer.A`'s members are
// converted, `Outer.B` has not been created yet, so its name still resolves to
// null at that moment. It is `Loader::resolvePendingNamed`, the retry after the
// whole module is registered, that binds it — the same order-independence
// #1487 gave the top-level pass.
BEZEL_TEST(xmod_nested_type_in_signature_3540,
           sibling_nested_type_declared_later_in_the_same_enclosing_type) {
    RunOut r = buildAndRun(
        "module LibB; "
        "public class Outer "
        "  public class A "
        "    private _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method Value(): int => _v; "
        "    public method Combine(b: Outer.B): int => _v + b.N(); "
        "  end A; "
        "  public class B "
        "    private _n: int; "
        "    public constructor(n: int) _n := n; end constructor; "
        "    public method N(): int => _n; "
        "  end B; "
        "end Outer; ",
        "module App3; import LibB; "
        "class Program public static method Main(): int "
        "  using var a := new Outer.A(20); "
        "  using var b := new Outer.B(3); "
        "  Console.WriteLine($\"sib={a.Combine(b)}\"); "
        "  return 0; "
        "end Main; end Program; ", "sibling");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("sib=23\n"));
}

// per spec §11.6, v0.285, #3540 — a TOP-LEVEL type declared BEFORE the
// enclosing type names the nested type. `Front` is `md.types[0]` and `Outer` is
// `md.types[1]`, so `Outer.Box` does not exist when `Front.Take`'s signature is
// converted. The companion of the sibling row above, from the other direction,
// and the reason the retry is not scoped to nested types' own members.
BEZEL_TEST(xmod_nested_type_in_signature_3540,
           nested_type_named_by_a_top_level_type_declared_before_it) {
    RunOut r = buildAndRun(
        "module LibF; "
        "public class Front "
        "  public static method Take(b: Outer.Box): int => b.Value(); "
        "end Front; "
        "public class Outer "
        "  public class Box "
        "    private _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method Value(): int => _v; "
        "  end Box; "
        "end Outer; ",
        "module App2; import LibF; "
        "class Program public static method Main(): int "
        "  using var a := new Outer.Box(9); "
        "  Console.WriteLine($\"fwd={Front.Take(a)}\"); "
        "  return 0; "
        "end Main; end Program; ", "forward");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("fwd=9\n"));
}

// per spec §11.6, v0.285, #3540 — "Nested types may themselves contain nested
// types" (§11.6). A three-deep `A.B.C` composes the enclosing path on both
// sides, and the row additionally puts the nested type in a RETURN position
// (`Maker.Make(): owned A.B.C`), which is a different MetaType slot from a
// parameter and lowers through the same `lookupNamed`. A two-level-only fix
// answers "A.B" here and misses.
BEZEL_TEST(xmod_nested_type_in_signature_3540,
           three_level_nesting_and_a_nested_return_type) {
    RunOut r = buildAndRun(
        "module LibD; "
        "public class A "
        "  public class B "
        "    public class C "
        "      private _v: int; "
        "      public constructor(v: int) _v := v; end constructor; "
        "      public method Value(): int => _v; "
        "      public method Plus(o: A.B.C): int => _v + o.Value(); "
        "    end C; "
        "  end B; "
        "end A; "
        "public class Maker "
        "  public static method Make(v: int): owned A.B.C => new A.B.C(v); "
        "end Maker; ",
        "module AppD; import LibD; "
        "class Program public static method Main(): int "
        "  using var a := Maker.Make(30); "
        "  using var b := new A.B.C(4); "
        "  Console.WriteLine($\"deep={a.Plus(b)}\"); "
        "  return 0; "
        "end Main; end Program; ", "deep");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("deep=34\n"));
}

// per spec §2.8.3 rule 3 + §11.6, v0.285, #3540 (and #3529) — THE ROW THAT PINS
// THE KEY. `Outer1.Box` and `Outer2.Box` are two unrelated types under rule 3;
// each has a method taking ITS OWN type. Resolving both by the simple name
// "Box" would bind one signature to the other's TypeSymbol, and because the two
// hold DIFFERENT payloads (an `int` against a `string`) that is not a wrong
// number, it is one type's `bz.string*` read as the other's `int`. Both answers
// are asserted in one line so a swap cannot be hidden by a coincidence.
BEZEL_TEST(xmod_nested_type_in_signature_3540,
           same_simple_name_under_two_enclosing_types_keep_their_own_signatures) {
    RunOut r = buildAndRun(
        "module LibI; "
        "public class Outer1 "
        "  public class Box "
        "    private _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method Value(): int => _v; "
        "    public method Plus(o: Outer1.Box): int => _v + o.Value(); "
        "  end Box; "
        "end Outer1; "
        "public class Outer2 "
        "  public class Box "
        "    private _s: string; "
        "    public constructor(s: string) _s := s; end constructor; "
        "    public method Text(): string => _s; "
        "    public method Join(o: Outer2.Box): string => _s + o.Text(); "
        "  end Box; "
        "end Outer2; ",
        "module AppI; import LibI; "
        "class Program public static method Main(): int "
        "  using var a := new Outer1.Box(7); "
        "  using var b := new Outer1.Box(5); "
        "  using var p := new Outer2.Box(\"ab\"); "
        "  using var q := new Outer2.Box(\"cd\"); "
        "  Console.WriteLine($\"n={a.Plus(b)} s={p.Join(q)}\"); "
        "  return 0; "
        "end Main; end Program; ", "identity");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("n=12 s=abcd\n"));
}

// -- Controls: these pass on BOTH trees ------------------------------------

// CONTROL (spec §11.6, v0.285) — a TOP-LEVEL class with a self-typed
// parameter. Measured working on fca2f02, and it is what pins the non-nested
// world as unchanged: a top-level type has an empty enclosing path, so
// `qualifiedTypeName()` returns exactly the simple name and its `namedName` is
// byte-identical on the wire to the pre-#3540 spelling.
BEZEL_TEST(xmod_nested_type_in_signature_3540,
           top_level_self_typed_parameter_control) {
    RunOut r = buildAndRun(
        "module LibT; "
        "public class Box "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Value(): int => _v; "
        "  public method Plus(other: Box): int => _v + other.Value(); "
        "end Box; ",
        "module AppC1; import LibT; "
        "class Program public static method Main(): int "
        "  using var a := new Box(7); "
        "  using var b := new Box(5); "
        "  Console.WriteLine($\"sum={a.Plus(b)}\"); "
        "  return 0; "
        "end Main; end Program; ", "toplevel");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("sum=12\n"));
}

// CONTROL (spec §11.6, v0.285) — a method ON a nested class taking a TOP-LEVEL
// parameter type. Measured working on fca2f02. Together with the row above it
// isolates the defect to "a nested type NAMED IN a signature": neither "methods
// on imported nested types" nor "self-typed parameters" was ever broken, and
// this row must keep passing or the fix has widened past its subject.
BEZEL_TEST(xmod_nested_type_in_signature_3540,
           nested_method_with_top_level_parameter_control) {
    RunOut r = buildAndRun(
        "module LibS2; "
        "public class Tag "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method N(): int => _n; "
        "end Tag; "
        "public class Outer "
        "  public class Box "
        "    private _v: int; "
        "    public constructor(v: int) _v := v; end constructor; "
        "    public method Value(): int => _v; "
        "    public method PlusTag(t: Tag): int => _v + t.N(); "
        "  end Box; "
        "end Outer; ",
        "module AppC2; import LibS2; "
        "class Program public static method Main(): int "
        "  using var a := new Outer.Box(7); "
        "  using var t := new Tag(5); "
        "  Console.WriteLine($\"sum={a.PlusTag(t)}\"); "
        "  return 0; "
        "end Main; end Program; ", "nestedtop");
    BEZEL_EXPECT_EQ(r.status, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("sum=12\n"));
}

}  // namespace bezel::module
