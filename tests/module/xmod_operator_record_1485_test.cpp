// Cross-module imported user-defined operator overload returning a VALUE type
// (record) — issue #1485 (§25 operator overloading, §9 records, §2.5/§2.6
// build phases / import).
//
// Bug #1485: an imported `public static method operator +(a: Box, b: Box): Box`
//   is miscompiled in the IMPORTER when its record-by-value result is consumed.
//   Root cause was sema, not codegen: in ExprTypecheck_Exprs.cpp typeBinary()
//   the BinaryExpr's result type is set only inside a
//   `dynamic_cast<ast::OperatorDecl*>(opm->decl)` branch, which yields null for
//   an imported operator (imported symbols carry no AST node — decl == nullptr).
//   With no sema type on the BinaryExpr, `var r := a + b` infers null, the
//   downstream `r.V` member access fails to resolve, and codegen silently drops
//   the whole expression (fieldAddr() → null) or reads garbage. The fix mirrors
//   the imported-regular-method fallback: when the operator is imported, take
//   the loader-stashed importedReturnType(opm). typeUnary() had the same gap.
//
// The IDENTICAL program with the record + operator declared in the SAME module
// compiles correctly (the CONTROL below), and an imported NON-operator static
// method returning the same record was already fine — so only the imported
// operator path was defective.

#include <cstdio>
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

// Per-process unique working dir under the shared scratch base. The previous
// std::filesystem::temp_directory_path() (== /tmp tmpfs) was race-prone even
// with unique_stem: parallel shards racing on the SAME /tmp parent dir
// overwrote each other's .bzl / .o mid-compileObject. The
// /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class
// COVERAGE SHORT). popen() at L51 is kept — per-stem uniqueness already
// mitigates the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir1485() {
    return wtScratchDir("wt-1485");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Run `exe`, capture its stdout, and return {exitCode, stdout}.
std::pair<int, std::string> runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>/dev/null").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// ─── §25 / §9 — CONTROL: same-module operator returning a record ─────────────

// The baseline that must not regress: a SAME-MODULE `operator +(Box,Box): Box`
// consumed via `r.V` prints 15 and returns 15.
BEZEL_TEST(xmod_operator_record_1485, same_module_operator_record_control_spec_25) {
    auto dir = wtDir1485();
    std::string stem = unique_stem("bezel_1485_sm_");
    TempFile aSrc((dir / (stem + ".bzl")).string());
    TempFile aExe((dir / (stem + "_a")).string());

    writeSource(aSrc.path,
        "module App; "
        "record Box(V: int) "
        "  public static method operator +(a: Box, b: Box): Box "
        "    => new Box(a.V + b.V); "
        "end Box; "
        "class Program "
        "  public static method Main(): int "
        "    var r := new Box(10) + new Box(5); "
        "    Console.WriteLine(r.V); "
        "    return r.V mod 256; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileFile(aSrc.path, aExe.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    auto [code, out] = runCapture(aExe.path);
    BEZEL_EXPECT_EQ(code, 15);
    BEZEL_EXPECT_EQ(out, std::string("15\n"));
}

// ─── §25 / §9 / §2.6 — Bug #1485: imported binary operator returning a record ─

// The core repro. An imported `operator +(Box,Box): Box`, its result consumed
// via `r.V` (direct field access into WriteLine) and via `var v := r.V`, must
// print 15 twice and return 15. Before the fix: the `Console.WriteLine(r.V)`
// line was silently dropped and `r.V` read 0.
BEZEL_TEST(xmod_operator_record_1485, imported_binary_operator_record_spec_25_9) {
    auto dir = wtDir1485();
    std::string stem = unique_stem("bezel_1485_bin_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Lib; "
        "public record Box(V: int) "
        "  public static method operator +(a: Box, b: Box): Box "
        "    => new Box(a.V + b.V); "
        "end Box;");
    writeSource(bSrc.path,
        "module App; "
        "import Lib; "
        "class Program "
        "  public static method Main(): int "
        "    var r := new Box(10) + new Box(5); "
        "    Console.WriteLine(\"A\"); "
        "    Console.WriteLine(r.V); "         // was silently dropped
        "    Console.WriteLine(\"B\"); "
        "    var v: int := r.V; "              // was read as 0
        "    Console.WriteLine(v); "
        "    Console.WriteLine(\"C\"); "
        "    return r.V mod 256; "             // was 0
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    auto [code, out] = runCapture(bExe.path);
    BEZEL_EXPECT_EQ(code, 15);
    BEZEL_EXPECT_EQ(out, std::string("A\n15\nB\n15\nC\n"));
}

// ─── §25 / §9 / §2.6 — Bug #1485: imported operator on a MULTI-FIELD record ───

// A multi-field record widened the symptom (uninitialized stack garbage, not
// just 0). Both fields of the imported operator result must materialize.
BEZEL_TEST(xmod_operator_record_1485, imported_operator_multifield_record_spec_25_9) {
    auto dir = wtDir1485();
    std::string stem = unique_stem("bezel_1485_mf_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Lib; "
        "public record Vec2(X: int, Y: int) "
        "  public static method operator +(a: Vec2, b: Vec2): Vec2 "
        "    => new Vec2(a.X + b.X, a.Y + b.Y); "
        "end Vec2;");
    writeSource(bSrc.path,
        "module App; "
        "import Lib; "
        "class Program "
        "  public static method Main(): int "
        "    var r := new Vec2(10, 3) + new Vec2(5, 4); "  // (15, 7)
        "    Console.WriteLine(r.X); "
        "    Console.WriteLine(r.Y); "
        "    return r.X + r.Y; "                            // 22
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    auto [code, out] = runCapture(bExe.path);
    BEZEL_EXPECT_EQ(code, 22);
    BEZEL_EXPECT_EQ(out, std::string("15\n7\n"));
}

// ─── §25 / §9 / §2.6 — Bug #1485: imported UNARY operator returning a record ──

// typeUnary() had the identical gap; an imported unary `operator -` returning a
// record must materialize its result.
BEZEL_TEST(xmod_operator_record_1485, imported_unary_operator_record_spec_25_9) {
    auto dir = wtDir1485();
    std::string stem = unique_stem("bezel_1485_un_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Lib; "
        "public record Box(V: int) "
        "  public static method operator -(a: Box): Box "
        "    => new Box(0 - a.V); "
        "end Box;");
    writeSource(bSrc.path,
        "module App; "
        "import Lib; "
        "class Program "
        "  public static method Main(): int "
        "    var r := -new Box(15); "  // Box(-15)
        "    Console.WriteLine(r.V); "  // -15
        "    return (0 - r.V) mod 256; " // 15
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    auto [code, out] = runCapture(bExe.path);
    BEZEL_EXPECT_EQ(code, 15);
    BEZEL_EXPECT_EQ(out, std::string("-15\n"));
}

// ─── §25 / §2.6 — CONTROL: imported operator returning a PRIMITIVE ───────────

// The narrowing control from the issue: an imported operator returning a
// primitive `int` was already correct and must stay correct (the fix is scoped
// to the value-return path but touches all imported operators).
BEZEL_TEST(xmod_operator_record_1485, imported_operator_primitive_control_spec_25) {
    auto dir = wtDir1485();
    std::string stem = unique_stem("bezel_1485_prim_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module Lib; "
        "public record Vec2(X: int, Y: int) "
        "  public static method operator *(a: Vec2, b: Vec2): int "
        "    => a.X * b.X + a.Y * b.Y; "
        "end Vec2;");
    writeSource(bSrc.path,
        "module App; "
        "import Lib; "
        "class Program "
        "  public static method Main(): int "
        "    var d := new Vec2(2, 3) * new Vec2(4, 5); "  // 8 + 15 = 23
        "    Console.WriteLine(d); "
        "    return d mod 256; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    auto [code, out] = runCapture(bExe.path);
    BEZEL_EXPECT_EQ(code, 23);
    BEZEL_EXPECT_EQ(out, std::string("23\n"));
}

}  // namespace
}  // namespace bezel::module
