// narrow #4561 — spec §32.13 / §2.8.2, v0.306, across a module boundary.
//
// §32.13 clause 1's C-compatible property is a §3.6 DERIVED property, and
// §3.6's common rules make it "part of the public contract". §2.8.2 clause 1
// requires the artifact interface to be sufficient to type-check every
// well-formed use of the public surface — so a record exported by module A
// must classify identically when module B names it in an `extern` signature.
//
// THE SEAM THIS ROW GUARDS (#3152). `ModuleLoader` never assigns
// `Symbol::decl` for an imported type: there is no `ast::TypeDecl` to assign.
// So the natural way to write the clause-1 walk — reach the record's
// `ast::TypeDecl` and iterate `positionalFields` — answers "not C-compatible"
// for EVERY imported record, silently and with a diagnostic that says the
// fields are wrong when the fields are fine. The shipped predicate instead
// reads `TypeSymbol::kind` / `::genericParams` / `::members` and takes each
// field's type from `sema::storageComponentType`, the both-sources accessor,
// so the two sides agree.
//
// The row exercises BOTH admission forms over imported records at once:
// clause 3's by-address `out` (a two-`long` `TimeSpec`, the spec's own
// `clock_gettime` example) and clause 4's typed handle by value and as a
// return (`Fd(Raw: int)` through `dup`). A same-module twin of each lives in
// tests/codegen/ffi_c_compatible_records_4561_test.cpp; this file is the
// boundary control, so a future AST-gated rewrite of the predicate fails here
// rather than shipping.

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

namespace {

const std::filesystem::path& wtDir4561() {
    return wtScratchDir("wt-ffi-xmod-4561");
}

void writeFile(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

}  // namespace

BEZEL_TEST(ffi_imported_c_record_4561,
           imported_record_classifies_c_compatible_at_extern_spec_32_13_4561) {
    auto dir = wtDir4561();
    std::string stem = unique_stem("bezel_4561_");
    std::string aSrc = (dir / (stem + "_a.bzl")).string();
    std::string aObj = (dir / (stem + "_a.o")).string();
    std::string bSrc = (dir / (stem + "_b.bzl")).string();
    std::string bExe = (dir / (stem + "_b")).string();

    // Module A exports both record shapes. Neither is mentioned in an extern
    // here — the classification has to survive the artifact round trip.
    writeFile(aSrc,
        "module FfiRecLib4561; "
        "public record TimeSpec(Sec: long, Nsec: long); "
        "public record Fd(Raw: int);");

    // Module B declares the externs over A's records and calls them.
    // clock_gettime(CLOCK_MONOTONIC) fills tv_sec with the uptime; dup(1)
    // returns a fresh descriptor above stderr.
    writeFile(bSrc,
        "module FfiRecMain4561; "
        "import FfiRecLib4561; "
        "extern \"libc\" "
        "  method clock_gettime(clockId: int, out ts: TimeSpec): int; "
        "  method dup(oldfd: Fd): Fd; "
        "  method close(fd: Fd): int; "
        "end; "
        "public class Program "
        "  public static method Main(): int "
        "    var ts: TimeSpec := new TimeSpec(0L, 0L); "
        "    var rc := clock_gettime(1, out ts); "
        "    if rc <> 0 then return 1; end; "
        "    if ts.Sec <= 0L then return 2; end; "
        "    if ts.Nsec < 0L then return 3; end; "
        "    if ts.Nsec >= 1000000000L then return 4; end; "
        "    var d := dup(new Fd(1)); "
        "    if d.Raw <= 2 then return 5; end; "
        "    if close(d) <> 0 then return 6; end; "
        "    return 0; "
        "  end Main; "
        "end Program;");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc, aObj);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;

    std::vector<bezel::Dependency> deps;
    {
        bezel::Dependency d;
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
    }
    // Pre-fix this compile FAILS outright: `isFfiCompatibleType` had no record
    // arm at all, so `out ts: TimeSpec` and `oldfd: Fd` were both rejected as
    // "not permitted at the FFI boundary (§32.5)".
    bezel::CompileResult rb = driver.compileFile(bSrc, bExe, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;

    int status = std::system(bExe.c_str());
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    // 0 = every check passed. 1 = clock_gettime failed; 2-4 = the `out`
    // record came back unwritten or out of range (a by-value lowering);
    // 5 = the typed handle did not marshal as its `int` field; 6 = the
    // returned handle was not a real descriptor.
    BEZEL_EXPECT_EQ(code, 0);

    std::remove(aSrc.c_str());
    std::remove(aObj.c_str());
    std::remove(bSrc.c_str());
    std::remove(bExe.c_str());
}
