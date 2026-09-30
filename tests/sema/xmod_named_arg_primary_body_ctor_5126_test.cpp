// Regression pin for #5126 — the §22.5 named-argument diagnostics
// (duplicate named argument / unknown name / missing required) at
// `new C(...)` must hold for an IMPORTED class that has a primary
// constructor beside a body constructor, exactly as for the local twin
// (#5121). Spec §22.5 "Optional parameters and named arguments", §22.7,
// §22.10 (v0.313).
//
// #5126 asked for the local-only `t->decl` read in the #5121 deferral
// (ExprTypecheck_Calls.cpp) to be replaced by a symbol-side read. Measured
// on origin/main d1827572: the imported class never reaches that deferral
// (`hasExplicitCtor && !isImported`); it takes the #3625 arm, which already
// binds over `FormalsView::of(ConstructorSymbol)` / the primary's positional
// symbols, so these rows PASS ON BOTH SIDES of the fix. They are a
// cross-module contract pin, not a fail-without; the fix itself removes the
// AST-decl read (ratchet baseline drop, scripts/imported_symbol_reads.baseline).

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::sema {
namespace {

// Per-process unique working dir under the shared scratch base (same
// rationale as the sibling imported-ctor test).
const std::filesystem::path& wtDir5126() {
    return wtScratchDir("wt-5126");
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

struct RunResult { int code; std::string out; };

RunResult runCapture(const std::string& exe, const std::string& args = "") {
    std::string out;
    FILE* p = popen((exe + (args.empty() ? "" : " " + args) +
                     " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::filesystem::path& dir,
                   const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem("bezel_5126lib_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct InProcessApp {
    bool ok = false;
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
};

InProcessApp buildApp(const std::filesystem::path& dir,
                      const std::string& libObjPath,
                      const std::string& appSrc) {
    InProcessApp r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem("bezel_5126app_");
    r.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = libObjPath; d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}


namespace {
const std::string kLib5126 =
    "module XLib5126; "
    "public class C(x: int) "
    "  public constructor(y: int, w: int) "
    "    this(y + w); "
    "  end constructor; "
    "  public method Show() "
    "    Console.WriteLine(x.ToString()); "
    "  end Show; "
    "end C; ";

bool compileApp5126(const std::string& stmt, std::string* outText = nullptr) {
    auto dir = wtDir5126();
    LibHandle libH = buildLib(dir, kLib5126);
    if (!libH.ok) return false;
    const std::string app =
        "module XMain5126; import XLib5126; class P "
        "  public static method Main(): int " + stmt +
        "    return 0; end Main; end P; ";
    InProcessApp r = buildApp(dir, libH.objPath, app);
    if (r.ok && outText) *outText = runCapture(r.exePath).out;
    return r.ok;
}
}  // namespace

BEZEL_TEST(xmod_named_arg_primary_body_ctor_5126,
           unknown_name_rejected_spec_22_5) {
    BEZEL_EXPECT_FALSE(compileApp5126("using var c := new C(z := 1); "));
}

BEZEL_TEST(xmod_named_arg_primary_body_ctor_5126,
           duplicate_named_arg_rejected_spec_22_5) {
    BEZEL_EXPECT_FALSE(compileApp5126("using var c := new C(x := 1, x := 2); "));
}

BEZEL_TEST(xmod_named_arg_primary_body_ctor_5126,
           named_arg_binding_primary_accepted_spec_22_5) {
    std::string out;
    BEZEL_EXPECT_TRUE(compileApp5126(
        "using var c := new C(x := 7); c.Show(); ", &out));
    BEZEL_EXPECT_TRUE(out.find("7") != std::string::npos);
}

}  // namespace
}  // namespace bezel::sema
