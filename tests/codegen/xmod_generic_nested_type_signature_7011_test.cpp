// Regression test for issue #7011 -- a generic type whose public signature
// names its own NESTED type must not break its importers (spec §11.6 "Nested
// types", §2.8.3 rule 3, v0.340).
//
// §2.8.3 rule 3: the artifact interface carries enough to reconstruct every
// public signature. Root cause (MEASURED): ModuleLoader pass 1 registered the
// nested-type shells of `G<T>` under "G.N" (parent.genericParams is only filled
// by pass 2) while the exporter spells the type "G.1.N", so the importer's
// by-name lookup missed and codegen reported "cannot lower imported Named type
// `G.1.N`" for EVERY importer, even one that never names G.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::codegen {
namespace {

const std::filesystem::path& wtDir7011() {
    return wtScratchDir("wt-7011");
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

RunResult runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
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
    std::string stem = unique_stem("bezel_7011lib_");
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
    std::string stem = unique_stem("bezel_7011app_");
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

}  // namespace

BEZEL_TEST(xmod_generic_nested_type_signature_7011, empty_importer_prints_1_spec_11_6) {
    auto dir = wtDir7011();
    const std::string lib = "module Lib7011a; public class G<T> public class N public method Get(): int return 7; end Get; end N; public static method Make(): owned N return new N(); end Make; end G; ";
    const std::string app = "module App7011a; import Lib7011a; class D public static method Main(): int Console.WriteLine(1); return 0; end Main; end D; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, "1\n");
}

BEZEL_TEST(xmod_generic_nested_type_signature_7011, factory_result_member_call_spec_11_6) {
    auto dir = wtDir7011();
    const std::string lib = "module Lib7011b; public class G<T> public class N public method Get(): int return 7; end Get; end N; public static method Make(): owned N return new N(); end Make; end G; ";
    const std::string app = "module App7011b; import Lib7011b; class D public static method Main(): int var n := G<int>.Make(); Console.WriteLine(n.Get()); return 0; end Main; end D; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, "7\n");
}

BEZEL_TEST(xmod_generic_nested_type_signature_7011, nested_record_and_two_levels_spec_11_6) {
    auto dir = wtDir7011();
    const std::string lib = "module Lib7011c; public class G<T> public record P(X: int); public class M public class N public method Get(): int return 3; end Get; end N; end M; public static method MakeP(): P return new P(4); end MakeP; public static method MakeN(): owned M.N return new M.N(); end MakeN; end G; ";
    const std::string app = "module App7011c; import Lib7011c; class D public static method Main(): int var p := G<int>.MakeP(); var n := G<int>.MakeN(); Console.WriteLine(p.X + n.Get()); return 0; end Main; end D; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, "7\n");
}

BEZEL_TEST(xmod_generic_nested_type_signature_7011, nested_param_and_loop_spec_11_6) {
    auto dir = wtDir7011();
    const std::string lib = "module Lib7011d; public class G<K, V> public class N public method Get(): int return 7; end Get; end N; public static method Take(n: N): int return n.Get() + 1; end Take; public static method Make(): owned N return new N(); end Make; end G; ";
    const std::string app = "module App7011d; import Lib7011d; class D public static method Main(): int var s := 0; for var i in 0..<100 do var n := G<int, string>.Make(); s := s + G<int, string>.Take(n); end; Console.WriteLine(s); return 0; end Main; end D; ";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp r = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture(r.exePath);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, "800\n");
}

}  // namespace bezel::codegen
