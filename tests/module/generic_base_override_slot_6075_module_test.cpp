// Spec-derived regression test for issue #6075 -- the CROSS-MODULE half.
//
// §12.1 / §14.1 / §27.8, v0.338: a derived class overriding a virtual member of
// a generic base with the base's type ARGUMENT spelled out (`D: G<int>`,
// `D<U>: G<U>`, `D<U>: G<Stack<U>>`) must reuse the base member's vtable slot,
// also when the generic base is IMPORTED. The single-module rows live in
// tests/codegen/generic_base_override_slot_6075_test.cpp.
//
// The library `Lib` declares `G<T>` with `Foo(x: Array<T>)`, `Bar(x: T)`,
// `Baz(x: Box<T>)` and the indexer `[i: Array<T>]`; it is compiled with `Driver::compileObject`. The
// importer overrides them and calls them through a base-typed receiver. Expected
// output: `2 20 200` (one per line) for `D: G<int>` and `D<U>: G<U>`, and
// `30 20` for `D<U>: G<Stack<U>>` (which overrides `Baz`/`Bar` only: an
// `Array<Stack<int>>` local leaks on the base compiler too, so it is not used). The
// producer content revision stamp (kProducerContentRevision) matters here: the
// library and the importer must agree on the vtable slot keys, so both are built
// by this compiler.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

const char* kLib6075 =
    "module Lib;\n"
    "public class Box<T> end Box;\n"
    "public class G<T>\n"
    "  public virtual method Foo(x: Array<T>): int return 1; end Foo;\n"
    "  public virtual method Bar(x: T): int return 10; end Bar;\n"
    "  public virtual property [i: Array<T>]: int\n"
    "    get => 100;\n"
    "  end;\n"
    "  public virtual method Baz(x: Box<T>): int return 3; end Baz;\n"
    "end G;\n";

const char* kLibWitness6075 =
    "module Lib;\n"
    "public class Item end Item;\n"
    "public class Box<T> end Box;\n"
    "public interface IG<T>\n"
    "  method Foo(x: T): int;\n"
    "end IG;\n"
    "public class C<T>: IG<T>\n"
    "  public method Foo(x: int): int return 1; end Foo;\n"
    "  public method Foo(x: T): int return 2; end Foo;\n"
    "end C;\n";

struct TempFile6075 {
    std::string path;
    explicit TempFile6075(std::string p) : path(std::move(p)) {}
    ~TempFile6075() { std::remove(path.c_str()); }
};

void writeSource6075(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Builds the library object and the importer (generated-code ASan on for the
// importer), runs the importer and returns its captured stdout. Returns false
// with `err` set when a build step fails.
bool compileImportAndRun6075(const std::string& libSrc, const std::string& appSrc,
                             std::string& stdoutText, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz6075_");
    TempFile6075 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile6075 aObj((dir / (stem + "_a.o")).string());
    writeSource6075(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) {
            err = "library compileObject failed";
            return false;
        }
    }

    TempFile6075 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile6075 bExe((dir / stem).string());
    TempFile6075 outFile((dir / (stem + "_out.txt")).string());
    writeSource6075(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps, opts).success) {
        err = "importer compileFile failed";
        return false;
    }
    std::string cmd = bExe.path + " > " + outFile.path;
    int status = std::system(cmd.c_str());
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        err = "importer did not exit 0";
        return false;
    }
    std::ifstream in(outFile.path);
    std::ostringstream ss;
    ss << in.rdbuf();
    stdoutText = ss.str();
    return true;
}

}  // namespace

// §12.1 / §14.1 / §27.8, v0.338: `D: G<int>` overrides, in the importing module, the
// generic base's members declared by the library (Foo(x: Array<int>), Bar(x: int), [i: Array<int>]). Each override must
// reuse the imported base's vtable slot, so a base-typed call reaches it.
// Pre-fix stdout differs (the bare-`T` member kept the base body, `10`).
BEZEL_TEST(generic_base_override_slot_6075_module,
           two_module_imported_generic_base_override_int_12_1_v0338) {
    const char* app =
        "module App; import Lib;\n"
        "public class D: G<int>\n"
        "  public override method Foo(x: Array<int>): int return 2; end Foo;\n"
        "  public override method Bar(x: int): int return 20; end Bar;\n"
        "  public override property [i: Array<int>]: int\n"
        "    get => 200;\n"
        "  end;\n"
        "end D;\n"
        "public class M\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    var a := new Array<int>(1);\n"
        "    Console.WriteLine(b.Foo(a).ToString());\n"
        "    Console.WriteLine(b.Bar(3).ToString());\n"
        "    Console.WriteLine(b[a].ToString());\n"
        "  end Main;\n"
        "end M;\n";
    std::string out, err;
    BEZEL_EXPECT_TRUE(compileImportAndRun6075(kLib6075, app, out, err));
    BEZEL_EXPECT_EQ(out, std::string("2\n20\n200\n"));
}

// §12.1 / §14.1 / §27.8, v0.338: `D<U>: G<U>` overrides, in the importing module, the
// generic base's members declared by the library (Array<U>/U). Each override must
// reuse the imported base's vtable slot, so a base-typed call reaches it.
// Pre-fix stdout differs (the bare-`T` member kept the base body, `10`).
BEZEL_TEST(generic_base_override_slot_6075_module,
           two_module_imported_generic_base_override_type_parameter_12_1_v0338) {
    const char* app =
        "module App; import Lib;\n"
        "public class D<U>: G<U>\n"
        "  public override method Foo(x: Array<U>): int return 2; end Foo;\n"
        "  public override method Bar(x: U): int return 20; end Bar;\n"
        "  public override property [i: Array<U>]: int\n"
        "    get => 200;\n"
        "  end;\n"
        "end D;\n"
        "public class M\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D<int>();\n"
        "    var a := new Array<int>(1);\n"
        "    Console.WriteLine(b.Foo(a).ToString());\n"
        "    Console.WriteLine(b.Bar(3).ToString());\n"
        "    Console.WriteLine(b[a].ToString());\n"
        "  end Main;\n"
        "end M;\n";
    std::string out, err;
    BEZEL_EXPECT_TRUE(compileImportAndRun6075(kLib6075, app, out, err));
    BEZEL_EXPECT_EQ(out, std::string("2\n20\n200\n"));
}

// §12.1 / §14.1 / §27.8, v0.338: `D<U>: G<Stack<U>>` overrides, in the importing module, the
// generic base's members declared by the library (Array<Stack<U>>/Stack<U>). Each override must
// reuse the imported base's vtable slot, so a base-typed call reaches it.
// Pre-fix stdout differs (the bare-`T` member kept the base body, `10`).
BEZEL_TEST(generic_base_override_slot_6075_module,
           two_module_imported_generic_base_override_nested_argument_12_1_v0338) {
    const char* app =
        "module App; import Lib;\n"
        "public class D<U>: G<Stack<U>>\n"
        "  public override method Baz(x: Box<Stack<U>>): int return 30; end Baz;\n"
        "  public override method Bar(x: Stack<U>): int return 20; end Bar;\n"
        "end D;\n"
        "public class M\n"
        "  public static method Main()\n"
        "    var b: G<Stack<int>> := new D<int>();\n"
        "    var bx := new Box<Stack<int>>();\n"
        "    var st := new Stack<int>();\n"
        "    Console.WriteLine(b.Baz(bx).ToString());\n"
        "    Console.WriteLine(b.Bar(st).ToString());\n"
        "  end Main;\n"
        "end M;\n";
    std::string out, err;
    BEZEL_EXPECT_TRUE(compileImportAndRun6075(kLib6075, app, out, err));
    BEZEL_EXPECT_EQ(out, std::string("30\n20\n"));
}

// §22.6 / §11.2 / §14 v0.338 (review of #6075): the interface-typed call on an imported
// `C<T>: IG<T>` whose `Foo(int)` is declared before `Foo(T)` reaches `Foo(T)` (prints 2).
BEZEL_TEST(generic_base_override_slot_6075_module,
           two_module_interface_witness_int_overload_before_t_22_6_v0338) {
    const char* app =
        "module App;\n"
        "import Lib;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var i: IG<string> := new C<string>();\n"
        "    Console.WriteLine(i.Foo(\"s\").ToString());\n"
        "  end Main;\n"
        "end X;\n";
    std::string out, err;
    BEZEL_EXPECT_TRUE(compileImportAndRun6075(kLibWitness6075, app, out, err));
    BEZEL_EXPECT_EQ(out, std::string("2\n"));
}

}  // namespace bezel::module
