// fix #3536 — §29.3 / §30.1 / §30.7 (v0.286): the sync-iterator, async-method,
// and async-iterator state-machine mints named their WHOLE machinery (five
// functions plus vtable globals) from a prefix built from the method's SIMPLE
// NAME alone:
//
//   LlvmBackend_Iterators.cpp:1169  "bz$iter$"      + ms->name + "$"
//   LlvmBackend_Async.cpp:1058      "bz$async$"     + ms->name + "$"
//   LlvmBackend_Async.cpp:2251      "bz$asynciter$" + ms->name + "$"
//
// Two classes that each declare a same-named iterator/async method therefore
// computed the SAME prefix. It was benign only because every mint at these
// sites is UNGUARDED (`llvm::Function::Create` / `new llvm::GlobalVariable`
// with no `getFunction`/`getNamedGlobal` existence check first), so LLVM's own
// name-uniquing silently handed the second class a private `.1`..`.7` copy of
// the machinery. The `.1`..`.7` suffixes are themselves the evidence that the
// unqualified prefix collided — see the issue's own measured IR dump.
//
// THE FIX qualifies each prefix with the owning class via the SAME
// `mangledTypeName` convention the ordinary per-class member spellings use
// (e.g. LlvmBackend_Setup.cpp's `"bz$" + mangledTypeName(ts) + "$Dispose"`):
// `"bz$iter$" + mangledTypeName(ms->parent) + "$" + ms->name + "$"`, and
// likewise for the async/asynciter families.
//
// THE ORACLE. Nothing behavioral distinguishes the pre-fix and post-fix
// compilers here — the issue's own program runs correctly on BOTH (LLVM's
// auto-uniquing papers over the collision today), so a program-output check
// would pass on the defect and prove nothing (an oracle that cannot observe
// the effect cannot certify the path). The direct, mechanical oracle is the
// EMITTED SYMBOL NAME itself (the same technique static_init_atomic_1391_test
// and spill_order_deterministic_3504_test use for a codegen-only change with
// no Bezel-level syntax to assert on): with the fix, the OLD unqualified
// spelling (`"bz$iter$Items$"`, `"bz$async$Compute$"`, `"bz$asynciter$Items$"`)
// never appears in the emitted module at all — every occurrence is qualified
// with the owning class — and each class's own qualified name appears
// distinctly, with no LLVM-appended `.N` disambiguation suffix. Reverting the
// fix (`git checkout HEAD~1 -- src/codegen/LlvmBackend_Iterators.cpp
// src/codegen/LlvmBackend_Async.cpp`) makes the "unqualified spelling absent"
// assertions fail and reproduces the issue's own measured `.1`/`.6`/`.7` sibling
// suffixes.
//
// Execution correctness (both classes' machinery independently reachable and
// correct) is checked too, as a secondary, defense-in-depth assertion — not
// the primary oracle, since it does not distinguish pre-fix from post-fix.

#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

// `bezelc` sits one level above this test binary in the same build tree —
// see spill_order_deterministic_3504_test.cpp / static_init_atomic_1391_test
// for the identical helper (copied verbatim per that file's own precedent
// comment: "helpers verbatim; only the slice marker and the source shape …
// differ").
std::string bezelcPath() {
    std::array<char, 4096> buf{};
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return "";
    fs::path exe(std::string(buf.data(), static_cast<std::size_t>(n)));
    return (exe.parent_path().parent_path() / "bezelc").string();
}

// Compile to an object with BEZEL_DUMP_IR=1 and capture the module text the
// compiler prints to stderr. A SUBPROCESS, not in-process Driver — LLVM's
// errs() is a raw_fd_ostream whose capture by redirecting this process's own
// fd 2 was found unreliable in #1391.
std::string compileAndCaptureIR(const std::string& source) {
    static int counter = 0;
    std::string bezelc = bezelcPath();
    if (bezelc.empty() || !fs::exists(bezelc)) return "";
    const char* envDir = std::getenv("BEZEL_AOT_SCRATCH_DIR");
    fs::path dir = envDir && *envDir ? fs::path(envDir)
                                     : fs::temp_directory_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string stem = "bezel_3536_" + std::to_string(getpid()) + "_" +
                       std::to_string(counter++);
    std::string src = (dir / (stem + ".bzl")).string();
    std::string obj = (dir / (stem + ".o")).string();
    {
        std::ofstream out(src);
        out << source;
    }
    std::string cmd = "BEZEL_DUMP_IR=1 '" + bezelc + "' -c -o '" + obj +
                      "' '" + src + "' 2>&1";
    std::string text;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        std::array<char, 8192> chunk{};
        std::size_t got;
        while ((got = fread(chunk.data(), 1, chunk.size(), p)) > 0)
            text.append(chunk.data(), got);
        pclose(p);
    }
    std::remove(src.c_str());
    std::remove(obj.c_str());
    return text;
}

// Two classes A and B each declare a SYNC iterator method named `Items`,
// mirroring the issue's own measured repro exactly (module M, classes A/B,
// same method name and shape). `A.Items` yields 1, 2; `B.Items` yields 4;
// summing both gives 7.
const char* kSyncIterCollisionSrc =
    "module M; "
    "class A "
    "  public method Items(): IEnumerable<int> "
    "    yield return 1; "
    "    yield return 2; "
    "  end Items; "
    "end A; "
    "class B "
    "  public method Items(): IEnumerable<int> "
    "    yield return 4; "
    "  end Items; "
    "end B; "
    "class Prog "
    "  public static method Main(): int "
    "    using var a := new A(); "
    "    using var b := new B(); "
    "    var n := 0; "
    "    for var x in a.Items() do n := n + x; end; "
    "    for var x in b.Items() do n := n + x; end; "
    "    return n; "
    "  end Main; "
    "end Prog;";

// Two classes A and B each declare a same-named ASYNC method `Compute`.
const char* kAsyncMethodCollisionSrc =
    "module M; "
    "class A "
    "  public async method Compute(): Task<int> "
    "    await Task.Delay(1); "
    "    return 3; "
    "  end Compute; "
    "end A; "
    "class B "
    "  public async method Compute(): Task<int> "
    "    await Task.Delay(1); "
    "    return 4; "
    "  end Compute; "
    "end B; "
    "class Prog "
    "  public static async method Main(): Task<int> "
    "    using var a := new A(); "
    "    using var b := new B(); "
    "    var x := await a.Compute(); "
    "    var y := await b.Compute(); "
    "    return x + y; "
    "  end Main; "
    "end Prog;";

// Two classes A and B each declare a same-named ASYNC ITERATOR method `Items`
// (LlvmBackend_Async.cpp's emitAsyncIteratorMethod — the issue explicitly
// calls out that this site "has the same shape and deserves the same check").
const char* kAsyncIterCollisionSrc =
    "module M; "
    "class A "
    "  public async method Items(): IAsyncEnumerable<int> "
    "    yield return 1; "
    "    await Task.Delay(1); "
    "    yield return 2; "
    "  end Items; "
    "end A; "
    "class B "
    "  public async method Items(): IAsyncEnumerable<int> "
    "    yield return 4; "
    "  end Items; "
    "end B; "
    "class Prog "
    "  public static async method Main(): Task<int> "
    "    using var a := new A(); "
    "    using var b := new B(); "
    "    var n := 0; "
    "    await for var x in a.Items() do n := n + x; end; "
    "    await for var x in b.Items() do n := n + x; end; "
    "    return n; "
    "  end Main; "
    "end Prog;";

}  // namespace

// --- Sync iterator family (LlvmBackend_Iterators.cpp emitIteratorMethod) ---
BEZEL_TEST(iterator_async_prefix_owner_qualifier_3536,
           sync_iter_prefix_qualified_by_owning_class) {
    std::string ir = compileAndCaptureIR(kSyncIterCollisionSrc);
    BEZEL_EXPECT_TRUE(!ir.empty());
    // FAIL-WITHOUT: the pre-fix compiler emits this unqualified spelling for
    // (at least) the first class's machinery — `bz$iter$Items$MoveNext` etc.
    // — and LLVM auto-uniques the second class's mint to `...MoveNext.1`.
    BEZEL_EXPECT_TRUE(ir.find("\"bz$iter$Items$") == std::string::npos);
    // PASS-WITH: each class's machinery is keyed by its OWN qualified name,
    // so both are present, distinct, and neither carries an auto-unique
    // suffix (a `.N` after `MoveNext"` would mean the base name collided).
    BEZEL_EXPECT_TRUE(ir.find("\"bz$iter$M.A$Items$MoveNext\"") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("\"bz$iter$M.B$Items$MoveNext\"") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("MoveNext.1") == std::string::npos);

    // Defense-in-depth: both classes' independently-keyed machinery still
    // runs correctly (1 + 2 + 4 = 7). Not the primary oracle (see file
    // header) — this passes on the pre-fix compiler too.
    auto r = runAot(kSyncIterCollisionSrc);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// --- Async method family (LlvmBackend_Async.cpp emitAsyncMethod) ---
BEZEL_TEST(iterator_async_prefix_owner_qualifier_3536,
           async_method_prefix_qualified_by_owning_class) {
    std::string ir = compileAndCaptureIR(kAsyncMethodCollisionSrc);
    BEZEL_EXPECT_TRUE(!ir.empty());
    BEZEL_EXPECT_TRUE(ir.find("\"bz$async$Compute$") == std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("\"bz$async$M.A$Compute$Resume\"") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("\"bz$async$M.B$Compute$Resume\"") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("Resume.1") == std::string::npos);

    auto r = runAot(kAsyncMethodCollisionSrc);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// --- Async iterator family (LlvmBackend_Async.cpp emitAsyncIteratorMethod) ---
BEZEL_TEST(iterator_async_prefix_owner_qualifier_3536,
           async_iter_prefix_qualified_by_owning_class) {
    std::string ir = compileAndCaptureIR(kAsyncIterCollisionSrc);
    BEZEL_EXPECT_TRUE(!ir.empty());
    BEZEL_EXPECT_TRUE(ir.find("\"bz$asynciter$Items$") == std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("\"bz$asynciter$M.A$Items$MoveNextAsync\"") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("\"bz$asynciter$M.B$Items$MoveNextAsync\"") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(ir.find("MoveNextAsync.1") == std::string::npos);

    auto r = runAot(kAsyncIterCollisionSrc);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
