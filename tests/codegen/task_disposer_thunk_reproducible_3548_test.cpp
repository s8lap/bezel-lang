// fix #3548 — §2.8.3 rule 3 (v0.286): a task/element/result disposer thunk's
// SYMBOL NAME must be a deterministic function of the type it disposes, not a
// raw AST/type POINTER value.
//
// THE DEFECT (issue #3548). `taskElementDisposerThunk` (LlvmBackend_Disposal.cpp,
// the issue's own repro site) built its symbol as
//   "bz$taskElemDispose" + "$" + std::to_string(reinterpret_cast<uintptr_t>(taskTy))
// — a raw heap address. ASLR randomizes every process's heap base, so
// compiling the SAME source with the SAME compiler in two separate processes
// emitted two DIFFERENT symbol names and therefore two DIFFERENT object files.
// Measured in the issue on b43196a: `for i in 1 2 3 4; do bezelc -c r.bzl -o
// r$i.o; done; md5sum r?.o` produced FOUR distinct hashes. Four more sites
// shared the same shape: nestedCollectionDisposerThunk's `elemCollTy` and the
// Pattern-A result-collection free thunk's `resultCollTy` (both
// LlvmBackend_Disposal.cpp), and the (ps, recvTy) pair naming the four
// imported-indexer/property inherited-wrapper thunks (LlvmBackend_Calls.cpp).
//
// WHY REPEAT-COMPILE **IS** THE RIGHT ORACLE HERE (unlike #3504 / #3550).
// Those two issues were pointer-keyed unordered_map ITERATION order defects:
// ASLR shifts every address by one common offset, so the pairwise
// differences that decide hash-bucket order are unchanged across runs, and a
// repeat-compile diff sees nothing (measured: 40/40 byte-identical objects
// over a live defect). This issue is different in kind — the pointer VALUE
// itself is serialized into the symbol name — so ASLR's per-process address
// shift is directly visible in the object bytes. Two compiles of the same
// source, in two separate processes, is exactly the oracle that catches it
// (LANE_RULES §18/§19).
//
// THE FIX. Each of the five sites now derives its disambiguating suffix from
// `Emitter::mangleTypeRefForSymbol` (LlvmBackend_impl.hpp, new), a recursive,
// pointer-free mangling of a `sema::TypeRef*` built on the existing
// `mangledTypeName(TypeSymbol*)` authority (§2.8.3 rule 3: "a constructed
// generic type is identified by its generic's identity plus the identities of
// its arguments") — mirroring the shape of the local `mangleTy` lambda
// `LlvmBackend_Setup.cpp`'s genericSubst path (#738) already uses, but as a
// header-level function shared across TUs. Any residual name collision (e.g.
// two distinct TypeRef* nodes that denote the SAME semantic type) is caught by
// llvm::Function::Create's Module insertion, which auto-renames via
// `ValueSymbolTable::createValueName` — deterministic on creation order, which
// is itself deterministic here (single-threaded, syntax-order codegen walk).
//
// MEASURED on this fix, same repro as the issue:
//   $ for i in 1 2 3 4; do bezelc -c r.bzl -o r$i.o; done; md5sum r?.o
//   b316d016655b44a7d3a0b243557edfaf  r1.o   (all four identical)
//   $ nm r1.o | grep taskElemDispose
//   bz$taskElemDispose$Task                  (was bz$taskElemDispose$24965040)

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

// `bezelc` sits one level above this test binary in the same build tree
// (<build>/tests/bezel_tests_codegen and <build>/bezelc). Resolved through
// /proc/self/exe — the same approach as spill_order_deterministic_3504 /
// capture_order_deterministic_3550.
//
// ⚠ Shared consequence: this row is an oracle on the `bezelc` BINARY, not on
// the `libbezel_core.a` this test links. If a build produces the test binary
// without relinking `bezelc`, this row measures a STALE compiler.
std::string bezelcPath() {
    std::array<char, 4096> buf{};
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return "";
    fs::path exe(std::string(buf.data(), static_cast<std::size_t>(n)));
    return (exe.parent_path().parent_path() / "bezelc").string();
}

// Reads a whole file's bytes. Empty on any failure (missing file, etc.) — the
// caller must separately assert the file is non-empty before trusting a
// byte-equality comparison, or a "both compiles failed" false pass is
// indistinguishable from a real match.
std::string readAllBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// Compiles `source` to a `.o` via a SEPARATE SUBPROCESS invocation of the real
// `bezelc -c` (not the in-process Driver) — a fresh process is exactly what
// gives each compile its own ASLR heap-base randomization, which is the
// condition under which the pre-fix pointer-derived name changed. Returns the
// object's raw bytes, or empty on failure.
std::string compileToObjectBytes(const std::string& source, int tag) {
    std::string bezelc = bezelcPath();
    if (bezelc.empty() || !fs::exists(bezelc)) return "";
    const char* envDir = std::getenv("BEZEL_AOT_SCRATCH_DIR");
    fs::path dir = envDir && *envDir ? fs::path(envDir)
                                     : fs::temp_directory_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string stem = "bezel_3548_" + std::to_string(getpid()) + "_" +
                       std::to_string(tag);
    std::string src = (dir / (stem + ".bzl")).string();
    std::string obj = (dir / (stem + ".o")).string();
    {
        std::ofstream out(src);
        out << source;
    }
    std::string cmd = "'" + bezelc + "' -c -o '" + obj + "' '" + src +
                      "' >/dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    std::string bytes;
    if (rc == 0) bytes = readAllBytes(obj);
    std::remove(src.c_str());
    std::remove(obj.c_str());
    return bytes;
}

// Issue #3548's own repro program verbatim: an async method throws, its Task
// is stashed in a `using var OwningArray<Task>()`, awaited and caught, and the
// array's scope-end disposal must free the caught (unconsumed) task element —
// exactly the `taskElementDisposerThunk` path the issue's `nm` diff pinned.
const char* kIssueRepro =
    "module M; class C\n"
    "  public static async method Boom(): Task\n"
    "    throw new InvalidOperationException(\"f\");\n"
    "  end Boom;\n"
    "  public static async method Main(): Task<int>\n"
    "    using var tasks := new OwningArray<Task>();\n"
    "    tasks.Add(Boom());\n"
    "    var x := 0;\n"
    "    try await tasks[0]; catch Exception => x := 55; end;\n"
    "    return x;\n"
    "  end Main;\n"
    "end C;\n";

// Two DISTINCT Task<T> instantiations disposed from two owning arrays in the
// SAME module — the shape #3548's fix must still discriminate (§2.8.3 rule
// 3's "generic's identity plus the identities of its arguments"): a scheme
// that named the thunk from the collection kind alone (dropping the pointer
// with nothing to replace it) would collapse `Task<int>` and `Task<string>`
// onto ONE thunk and dispose one T's result as if it were the other's.
const char* kTwoTaskInstantiations =
    "module M; class C\n"
    "  public static async method Get(): Task<int>\n"
    "    return 1;\n"
    "  end Get;\n"
    "  public static async method GetS(): Task<string>\n"
    "    return \"x\";\n"
    "  end GetS;\n"
    "  public static async method Main(): Task<int>\n"
    "    using var a := new OwningArray<Task<int>>();\n"
    "    a.Add(Get());\n"
    "    using var b := new OwningArray<Task<string>>();\n"
    "    b.Add(GetS());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

}  // namespace

// The core reproducibility pin: FOUR separate-process compiles of the issue's
// repro must produce byte-identical objects. Pre-fix (measured on the parent
// commit) this failed with FOUR distinct md5sums; the fix makes the
// taskElemDispose symbol name a pure function of the type, so every run
// produces the same bytes.
BEZEL_TEST(task_disposer_thunk_reproducible_3548, issue_repro_four_compiles_identical) {
    std::string r1 = compileToObjectBytes(kIssueRepro, 1);
    std::string r2 = compileToObjectBytes(kIssueRepro, 2);
    std::string r3 = compileToObjectBytes(kIssueRepro, 3);
    std::string r4 = compileToObjectBytes(kIssueRepro, 4);
    // Assert non-empty FIRST: an empty comparison (both compiles failed) would
    // otherwise pass this row vacuously (§3 LANE_RULES: "an oracle that
    // cannot observe the effect cannot certify the path").
    BEZEL_EXPECT_TRUE(!r1.empty());
    BEZEL_EXPECT_TRUE(!r2.empty());
    BEZEL_EXPECT_TRUE(!r3.empty());
    BEZEL_EXPECT_TRUE(!r4.empty());
    BEZEL_EXPECT_TRUE(r1 == r2);
    BEZEL_EXPECT_TRUE(r1 == r3);
    BEZEL_EXPECT_TRUE(r1 == r4);
    // The symbol itself must be present and must be the deterministic,
    // type-derived spelling — not a run of decimal digits (the old
    // `std::to_string(reinterpret_cast<uintptr_t>(taskTy))` shape). Object
    // symbol names are stored as plain ASCII in the ELF string table, so a
    // substring search over the raw bytes finds it without depending on an
    // external `nm` binary being on PATH.
    BEZEL_EXPECT_TRUE(r1.find("bz$taskElemDispose$Task") != std::string::npos);
}

// Type-argument discrimination: `Task<int>` and `Task<string>` disposed in
// the same module must get TWO DISTINCT thunk symbols, and the pair must
// itself be reproducible across two separate-process compiles.
BEZEL_TEST(task_disposer_thunk_reproducible_3548, distinct_task_instantiations_distinct_symbols) {
    std::string r1 = compileToObjectBytes(kTwoTaskInstantiations, 11);
    std::string r2 = compileToObjectBytes(kTwoTaskInstantiations, 12);
    BEZEL_EXPECT_TRUE(!r1.empty());
    BEZEL_EXPECT_TRUE(!r2.empty());
    BEZEL_EXPECT_TRUE(r1 == r2);
    BEZEL_EXPECT_TRUE(
        r1.find("bz$taskElemDispose$Task_int") != std::string::npos);
    BEZEL_EXPECT_TRUE(
        r1.find("bz$taskElemDispose$Task_string") != std::string::npos);
}

// Reordering/renaming the thunk must not change what the program computes:
// the issue's own repro program returns 55 (the catch-block assignment) both
// before and after the fix — this is a naming-only change.
BEZEL_TEST(task_disposer_thunk_reproducible_3548, issue_repro_value_preserved) {
    auto r = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}
