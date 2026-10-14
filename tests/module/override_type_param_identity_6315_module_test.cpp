// #6315 / #6316 (b) (§12.1, §11.2, v0.338) -- the CROSS-MODULE half of the
// override-signature rows in tests/sema/type_param_identity_6315_test.cpp.
//
// §12.1: "Visibility, return type, parameter types, and parameter modifiers
// must match the base member exactly" ... "An override declares exactly as
// many type parameters of its own as the overridden member ..., the two lists
// corresponding by position as in §11.2's *Generic members*". §11.2: "A
// member's own type parameters take part in the match by number and position,
// not by name."
//
// The base `VB<T>` (and `D<X>: VB<Array<X>>`, `GB<T>`) is IMPORTED: its member
// types arrive through the artifact interface, not an AST, so the comparison
// reads `resolvedDeclType` / `importedReturnType` / `importedMemberType`. A
// class type parameter has no declaration on either side, so the old
// declaration compare equated `Array<V>` with the substituted `Array<U>`;
// every reject row below compiled clean before the fix. The accept rows pin
// that the imported spellings still match the same parameter, renamed own
// type parameters, and a chain through an imported intermediate class.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

const char* kLib6315 =
    "module Lib6315;\n"
    "public class VB<T>\n"
    "  public constructor() end constructor;\n"
    "  public virtual method M(x: T): string => \"bm\";\n"
    "  public virtual method R(): T? => null;\n"
    "  public virtual property P: T? => null;\n"
    "  public virtual property [i: T]: string => \"bi\";\n"
    "  public virtual method Q<A>(x: A, y: T): string => \"bq\";\n"
    "end VB;\n"
    "public class D<X>: VB<Array<X>>\n"
    "  public constructor() end constructor;\n"
    "end D;\n"
    "public class GB<T>\n"
    "  public constructor() end constructor;\n"
    "  public virtual method G<A, C>(x: A, y: T): C? => null;\n"
    "end GB;\n";

struct TempFile6315 {
    std::string path;
    explicit TempFile6315(std::string p) : path(std::move(p)) {}
    ~TempFile6315() { std::remove(path.c_str()); }
};

void writeSource6315(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Outcome6315 {
    bool libBuilt = false;
    bool compiled = false;
    bool ran = false;
    std::string stderrText;
    std::string stdoutText;
};

// Builds the library object, compiles the importer with its stderr captured
// (the property under test is the §12.1 diagnostic), and, when it compiles,
// runs it and captures stdout.
Outcome6315 buildImporter6315(const std::string& appSrc,
                              const char* libSrc = kLib6315) {
    namespace fs = std::filesystem;
    Outcome6315 o;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz6315_");
    TempFile6315 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile6315 aObj((dir / (stem + "_a.o")).string());
    writeSource6315(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        o.libBuilt = driver.compileObject(aSrc.path, aObj.path).success;
    }
    if (!o.libBuilt) return o;

    TempFile6315 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile6315 bExe((dir / stem).string());
    TempFile6315 errFile((dir / (stem + "_err.txt")).string());
    TempFile6315 outFile((dir / (stem + "_out.txt")).string());
    writeSource6315(bSrc.path, appSrc);

    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errFile.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        o.compiled = bezel::test::compileFileForTest(driver, bSrc.path,
                                                     bExe.path, deps)
                         .success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    {
        std::ifstream in(errFile.path);
        o.stderrText.assign((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    }
    if (!o.compiled) return o;

    std::string cmd = bExe.path + " > " + outFile.path;
    int status = std::system(cmd.c_str());
    o.ran = status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::ifstream in(outFile.path);
    std::ostringstream ss;
    ss << in.rdbuf();
    o.stdoutText = ss.str();
    return o;
}

std::string app6315(const std::string& cls, const std::string& main = "") {
    return "module App6315;\nimport Lib6315;\n" + cls +
           "public class Program\n"
           "  public static method Main(): int\n" + main +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// #6321 (§27.8, §12.1, v0.338): an imported base whose member types are
// spelled through a generic alias of a generic alias. An alias is a pure
// synonym, so `Id2<X>` and `Id2<Id<X>>` are `X`. Before the fix `Id2<X>`
// resolved to the inner alias's unsubstituted parameter, and the exported
// member type carried that stale spelling.
const char* kLib6321 =
    "module Lib6321;\n"
    "type Id<A> := A;\n"
    "type Id2<T> := Id<T>;\n"
    "public class AB<X>\n"
    "  public constructor() end constructor;\n"
    "  public virtual method M(x: Id2<X>): string => \"bm\";\n"
    "  public virtual method R(x: X): Id2<X> => x;\n"
    "  public virtual method R2(x: X): Id2<Id<X>> => x;\n"
    "  public virtual property [i: Id2<X>]: string => \"bi\";\n"
    "end AB;\n";

std::string app6321(const std::string& cls, const std::string& main = "") {
    return "module App6321;\nimport Lib6321;\n" + cls +
           "public class Program\n"
           "  public static method Main(): int\n" + main +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

void expectRejected6315(const std::string& cls, const std::string& needle) {
    Outcome6315 o = buildImporter6315(app6315(cls));
    BEZEL_EXPECT_TRUE(o.libBuilt);
    BEZEL_EXPECT_FALSE(o.compiled);
    BEZEL_EXPECT_TRUE(o.stderrText.find(needle) != std::string::npos);
}

}  // namespace

// The imported `VB<Array<U>>`'s `M(x: T)` takes `Array<U>`; `Array<V>` is not it.
BEZEL_TEST(override_type_param_identity_6315_module,
           imported_base_param_nested_other_parameter_rejected_spec_12_1) {
    expectRejected6315(
        "public class E<U, V>: VB<Array<U>>\n"
        "  public constructor() end constructor;\n"
        "  public override method M(x: Array<V>): string => \"dm\";\n"
        "end E;\n",
        "override of `M` does not match the base member's parameter types (§12.1)");
}

// Return type of the imported `R(): T?` at `T = Array<U>`.
BEZEL_TEST(override_type_param_identity_6315_module,
           imported_base_return_nested_other_parameter_rejected_spec_12_1) {
    expectRejected6315(
        "public class E<U, V>: VB<Array<U>>\n"
        "  public constructor() end constructor;\n"
        "  public override method R(): Array<V>? => null;\n"
        "end E;\n",
        "override of `R` does not match the base member's return type (§12.1)");
}

// Key of the imported indexer `[i: T]` at `T = Array<U>`.
BEZEL_TEST(override_type_param_identity_6315_module,
           imported_base_indexer_key_nested_other_parameter_rejected_spec_12_1) {
    expectRejected6315(
        "public class E<U, V>: VB<Array<U>>\n"
        "  public constructor() end constructor;\n"
        "  public override property [i: Array<V>]: string => \"di\";\n"
        "end E;\n",
        "override of `indexer` does not match the base member's index parameter types (§12.1)");
}

// Through the imported intermediate `D<X>: VB<Array<X>>`: `E<Z, W>: D<Z>`
// binds `T` to `Array<Z>`, not `Array<W>`.
BEZEL_TEST(override_type_param_identity_6315_module,
           imported_chain_param_other_parameter_rejected_spec_12_1) {
    expectRejected6315(
        "public class E<Z, W>: D<Z>\n"
        "  public constructor() end constructor;\n"
        "  public override method M(x: Array<W>): string => \"em\";\n"
        "end E;\n",
        "override of `M` does not match the base member's parameter types (§12.1)");
}

// The imported `G<A, C>(x: A, y: T): C?`: own type parameters by position, so
// `G<P, Q>(...): P?` does not override it.
BEZEL_TEST(override_type_param_identity_6315_module,
           imported_base_own_type_parameter_wrong_position_rejected_spec_12_1) {
    expectRejected6315(
        "public class E<U>: GB<U>\n"
        "  public constructor() end constructor;\n"
        "  public override method G<P, Q>(x: P, y: U): P? => null;\n"
        "end E;\n",
        "override of `G` does not match the base member's return type (§12.1)");
}

// Control: every imported member overridden at the SAME parameter, the own
// type parameters renamed, and a chain through the imported `D`; dispatch
// through the base types reaches the overrides. (`Q` is only compiled, not
// called through `VB`: a base-typed call of an imported generic virtual method
// reaches the base body on the pre-fix compiler too, a separate defect.)
BEZEL_TEST(override_type_param_identity_6315_module,
           imported_base_same_parameter_and_renamed_own_parameters_accepted_spec_12_1) {
    Outcome6315 o = buildImporter6315(app6315(
        "public class E<U, V>: VB<Array<U>>\n"
        "  public constructor() end constructor;\n"
        "  public override method M(x: Array<U>): string => \"dm\";\n"
        "  public override method R(): Array<U>? => null;\n"
        "  public override property P: Array<U>? => null;\n"
        "  public override method Q<B>(x: B, y: Array<U>): string => \"dq\";\n"
        "end E;\n"
        "public class F<Z, W>: D<Z>\n"
        "  public constructor() end constructor;\n"
        "  public override method M(x: Array<Z>): string => \"fm\";\n"
        "end F;\n"
        "public class H<U>: GB<U>\n"
        "  public constructor() end constructor;\n"
        "  public override method G<P, Q>(x: P, y: U): Q? => null;\n"
        "end H;\n",
        "    var a: Array<int> := [1];\n"
        "    var b: VB<Array<int>> := new E<int, string>();\n"
        "    Console.WriteLine(b.M(a));\n"
        "    if b.R() = null then Console.WriteLine(\"null\"); end if;\n"
        "    var c: VB<Array<int>> := new F<int, bool>();\n"
        "    Console.WriteLine(c.M(a));\n"
        "    var g: GB<int> := new H<int>();\n"
        "    if g.G<string, string>(\"s\", 1) = null then Console.WriteLine(\"gnull\"); end if;\n"));
    BEZEL_EXPECT_TRUE(o.libBuilt);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("dm\nnull\nfm\ngnull\n"));
}

// #6321 (§27.8, §12.1, v0.338): the imported `AB<X>` spells its return types
// `Id2<X>` and `Id2<Id<X>>`, both of which are `X`, so `Q` overrides them. Before
// the fix the importer read `Id2<X>` as the alias's stale `T` and reported the
// return type as a mismatch. The calls go through the derived receiver and,
// at an `int` instantiation (a reference-type one is still the #783
// limitation), through the imported base receiver (#6323: the slot key).
BEZEL_TEST(override_type_param_identity_6315_module,
           imported_alias_of_alias_return_parameter_and_key_accepted_6321_spec_27_8) {
    Outcome6315 o = buildImporter6315(app6321(
        "public class E<Q>: AB<Q>\n"
        "  public constructor() end constructor;\n"
        "  public override method M(x: Q): string => \"dm\";\n"
        "  public override method R(x: Q): Q => x;\n"
        "  public override method R2(x: Q): Q => x;\n"
        "  public override property [i: Q]: string => \"di\";\n"
        "end E;\n",
        "    var e := new E<int>();\n"
        "    Console.WriteLine(e.M(1));\n"
        "    Console.WriteLine(e.R(7));\n"
        "    Console.WriteLine(e.R2(8));\n"
        "    Console.WriteLine(e[2]);\n"
        "    var b: AB<int> := e;\n"
        "    Console.WriteLine(b.M(1));\n"
        "    Console.WriteLine(b.R(7));\n"
        "    Console.WriteLine(b.R2(8));\n"
        "    Console.WriteLine(b[2]);\n"), kLib6321);
    BEZEL_EXPECT_TRUE(o.libBuilt);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_TRUE(o.stderrText.find("does not match the base member") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("dm\n7\n8\ndi\ndm\n7\n8\ndi\n"));
}

// #6321: with the alias read through, the imported `Id2<X>` is still `X`, not the
// derived class's other parameter `W`, at the return type, the parameter and the
// index key.
BEZEL_TEST(override_type_param_identity_6315_module,
           imported_alias_of_alias_return_other_parameter_rejected_6321_spec_12_1) {
    Outcome6315 o = buildImporter6315(app6321(
        "public class E<Q, W>: AB<Q>\n"
        "  public constructor() end constructor;\n"
        "  public override method R(x: Q): W => throw new Exception(\"y\");\n"
        "end E;\n"), kLib6321);
    BEZEL_EXPECT_TRUE(o.libBuilt);
    BEZEL_EXPECT_FALSE(o.compiled);
    BEZEL_EXPECT_TRUE(o.stderrText.find(
        "override of `R` does not match the base member's return type (§12.1)") !=
        std::string::npos);
}

BEZEL_TEST(override_type_param_identity_6315_module,
           imported_alias_of_alias_param_other_parameter_rejected_6321_spec_12_1) {
    Outcome6315 o = buildImporter6315(app6321(
        "public class E<Q, W>: AB<Q>\n"
        "  public constructor() end constructor;\n"
        "  public override method M(x: W): string => \"dm\";\n"
        "end E;\n"), kLib6321);
    BEZEL_EXPECT_TRUE(o.libBuilt);
    BEZEL_EXPECT_FALSE(o.compiled);
    BEZEL_EXPECT_TRUE(o.stderrText.find(
        "override of `M` does not match the base member's parameter types (§12.1)") !=
        std::string::npos);
}

BEZEL_TEST(override_type_param_identity_6315_module,
           imported_alias_of_alias_key_other_parameter_rejected_6321_spec_12_1) {
    Outcome6315 o = buildImporter6315(app6321(
        "public class E<Q, W>: AB<Q>\n"
        "  public constructor() end constructor;\n"
        "  public override property [i: W]: string => \"di\";\n"
        "end E;\n"), kLib6321);
    BEZEL_EXPECT_TRUE(o.libBuilt);
    BEZEL_EXPECT_FALSE(o.compiled);
    BEZEL_EXPECT_TRUE(o.stderrText.find(
        "override of `indexer` does not match the base member's index parameter "
        "types (§12.1)") != std::string::npos);
}

}  // namespace bezel::module
