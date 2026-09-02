// fix #3550 — §27.5 "Closures" / §30.6 "Async lambdas" (v0.286): the lambda
// capture frame's emit order must be REPRODUCIBLE.
//
// THE DEFECT. `emitAsyncLambda` (LlvmBackend_Async.cpp ~1636) iterated three
// pointer-keyed `std::unordered_map`s to build the `FrameCapture` list that
// drives the state-machine frame's CAPTURE_START field layout:
//
//   * `paramSlots_` — `std::unordered_map<const sema::Symbol*, llvm::AllocaInst*>`
//   * `locals_`     — `std::unordered_map<const ast::Node*,     llvm::AllocaInst*>`
//
// The iteration order of a POINTER-keyed unordered_map is decided by which
// key ADDRESSES collide modulo the (fixed) bucket prime, so any change to the
// compiler's own allocation order reorders the FrameCapture list and therefore
// the frame field layout and the GEP/load/store instructions that read or
// write those fields. Same mechanism, different seam — #3504's
// `smAllocaToField_` was the per-suspension group; this is the per-lambda
// capture set.
//
// WHY "COMPILE IT N TIMES AND COMPARE" IS NOT THE ORACLE. Same reason as
// #3504. The pairwise DIFFERENCES of key addresses are stable under ASLR (one
// common offset); only a real change to the compiler's allocation pattern
// (an unrelated `unordered_set::insert`, a different allocator, ASan, a
// different libc) shifts the iteration order. Measured on the parent
// compiler (9c1803): 40 compiles of #3504's repro in 40 separate processes
// produced 40 byte-identical objects. The defect is INVISIBLE to that oracle.
//
// THE ORACLE USED INSTEAD is the emitted ORDER itself, checkable on ONE
// compile. With the fix (MapVector) the captures iterate in INSERTION ORDER,
// which equals declaration order — locals_ and paramSlots_ are inserted as
// the AST is walked (lowerLocalDecl at LlvmBackend_Exprs2.cpp, ~8915/~8955,
// etc.), so MapVector iteration reproduces that order deterministically.
//
// The IR observable property is the sequence of `%preboxed*N = load i32, ptr
// %K` instructions the async Resume function emits, one per captured local.
// Each `%K` is the SSA value of the local's alloca; LLVM assigns SSA values
// in reverse-declaration order, so declaration order maps to a strictly
// DESCENDING sequence of `%K` values. Pre-fix the preboxed sequence reads
// `%17, %22, %23, %18, %24, %19, %25, %26, %20, %27, %21, %28` for the program
// below — a hash permutation, not a monotonic sequence. Post-fix it reads
// `%28, %27, %26, %25, %24, %23, %22, %21, %20, %19, %18, %17` — strictly
// descending, in the order locals were declared.
//
// Eight captures are enough that a hash order and a sorted order cannot
// coincide by accident (8! = 40320 permutations, and the pre-fix order above
// is measured NOT sorted).
//
// `smIterEnumFrameSlot_` (the fourth map the issue names) is REFUTED on this
// lane: it is only used for `find()` lookups at Stmts.cpp ~3790, never
// iterated. The defect class does not reach emitted IR on this map, and
// issue 3550's own triage rule ("a pre-scan that fills a set is fine and
// should be left alone rather than churned") applies. Recorded in the
// commit body; left alone in the implementation.

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <string>
#include <unistd.h>
#include <vector>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

// `bezelc` sits one level above this test binary in the same build tree
// (<build>/tests/bezel_tests_codegen and <build>/bezelc). Resolved through
// /proc/self/exe so the test works from any worktree's own build dir — the same
// approach as static_init_atomic_1391_test / spill_order_deterministic_3504.
std::string bezelcPath() {
    std::array<char, 4096> buf{};
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return "";
    fs::path exe(std::string(buf.data(), static_cast<size_t>(n)));
    return (exe.parent_path().parent_path() / "bezelc").string();
}

// Compile to an object with BEZEL_DUMP_IR=1 and capture the module text the
// compiler prints. A SUBPROCESS, not in-process Driver: LLVM's errs() is a
// raw_fd_ostream whose capture by redirecting this process's own fd 2 was
// found unreliable in #1391 / #3073 / #3504.
//
// ⚠ CONSEQUENCE, shared with the other IR-dump tests: this row is an oracle
// on the `bezelc` BINARY, not on the `libbezel_core.a` this test links. If a
// build produces the test binary without relinking `bezelc` (which
// `scripts/agent_check.sh` does, since it builds `bezel_tests` and only
// reaches `bezelc` in its later lit phase), the row measures a STALE compiler
// and fails for a reason that has nothing to do with the source under test.
std::string compileAndCaptureIR(const std::string& source) {
    static int counter = 0;
    std::string bezelc = bezelcPath();
    if (bezelc.empty() || !fs::exists(bezelc)) return "";
    const char* envDir = std::getenv("BEZEL_AOT_SCRATCH_DIR");
    fs::path dir = envDir && *envDir ? fs::path(envDir)
                                     : fs::temp_directory_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string stem = "bezel_3550_" + std::to_string(getpid()) + "_" +
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

// Slice one function body out of the module text, from its `define ... @"NAME"`
// line to the matching `\n}`.
//
// The DEFINE anchor is load-bearing: an async resume function is REFERENCED
// before it is defined (the entry function calls it, and `bz_task_then`
// takes its address), so a plain first-occurrence search for the mangled name
// lands in a different function's body and silently slices the wrong text.
std::string sliceFunctionBody(const std::string& ir, const std::string& name) {
    std::string marker = "@\"" + name + "\"";
    std::size_t start = std::string::npos;
    for (std::size_t at = ir.find(marker); at != std::string::npos;
         at = ir.find(marker, at + 1)) {
        std::size_t lineStart = ir.rfind('\n', at);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        if (ir.compare(lineStart, 6, "define") == 0) { start = at; break; }
    }
    if (start == std::string::npos) return "";
    std::size_t bodyStart = ir.find('{', start);
    if (bodyStart == std::string::npos) return "";
    std::size_t bodyEnd = ir.find("\n}", bodyStart);
    if (bodyEnd == std::string::npos) return "";
    return ir.substr(bodyStart, bodyEnd - bodyStart);
}

// Each lambda capture emits a `preboxed` cell-load block of the shape:
//
//   %preboxed = load i32, ptr %K, align 4   ; reads the LOCAL value
//   store i32 %preboxed, ptr %N, align 4    ; copies it into the cell
//   store ptr %N, ptr %M, align 8          ; publishes the cell pointer
//   br label %cell.done
//
// (repeat for every captured local in capture-frame order).
//
// We extract the %K values in EMITTED ORDER. With the MapVector fix the
// iteration is insertion-ordered, which is declaration-ordered, which LLVM
// reflects as a strictly DESCENDING sequence of SSA values (locals_ are
// allocated in declaration order; LLVM's name-table scans in reverse when it
// runs out of fresh N% values). Pre-fix the sequence is a hash permutation.
std::vector<unsigned> captureLoadAllocas(const std::string& body) {
    std::regex rx(R"(%preboxed(\d*) = load i32, ptr %(\d+), align 4)");
    std::vector<unsigned> out;
    auto begin = std::sregex_iterator(body.begin(), body.end(), rx);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        out.push_back(static_cast<unsigned>(
            std::stoul((*it)[2].str())));
    }
    return out;
}

bool strictlyDescending(const std::vector<unsigned>& v) {
    for (std::size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] <= v[i]) return false;
    return true;
}

// Eight locals declared a..h, captured by an async lambda after one `await`.
// Each local captures one slot in the state-machine frame; the lambda's body
// reads them via `a + b + ... + h` so all eight reach emitted IR. Eight is
// enough that a hash order cannot coincide with a sorted order by accident
// (8! = 40320 permutations).
const char* kEightCapturedLocalsAcrossAnAwait =
    "module M; import System; class C "
    "  public static async method Outer(): Task<int> "
    "    var a := 1; var b := 2; var c := 3; var d := 4; "
    "    var e := 5; var f := 6; var g := 7; var h := 8; "
    "    await Task.Delay(1); "
    "    var cap := lambda() => a + b + c + d + e + f + g + h; "
    "    return cap(); "
    "  end Outer; "
    "  public static method Main(): int "
    "    return C.Outer().Result; "
    "  end Main; "
    "end C;";

}  // namespace

// The lambda's per-capture cell-load sequence must read local allocas in
// strictly DESCENDING SSA-value order, because the MapVector fix makes the
// iteration insertion-ordered (= declaration-ordered) and LLVM names the
// locals' allocas in reverse-declaration order. Pre-fix the sequence is a
// hash permutation (measured: 17 22 23 18 24 19 25 26 20 27 21 28).
BEZEL_TEST(capture_order_deterministic_3550, cell_loads_are_in_declaration_order) {
    std::string ir = compileAndCaptureIR(kEightCapturedLocalsAcrossAnAwait);
    BEZEL_EXPECT_TRUE(!ir.empty());
    // fix #3536: the async Resume mangling includes the owning class
    // (`mangledTypeName(ms->parent)`, "M.C" for `module M; class C ...`) so
    // two same-named async methods on different classes no longer collide.
    std::string body = sliceFunctionBody(ir, "bz$async$M.C$Outer$Resume");
    BEZEL_EXPECT_TRUE(!body.empty());
    auto alloca_seq = captureLoadAllocas(body);
    // Asserting the COUNT is what stops this test from passing vacuously if
    // the emitted shape ever changes out from under it — a silent "no loads
    // found" would otherwise look identical to success.
    BEZEL_EXPECT_EQ(alloca_seq.size(), static_cast<std::size_t>(8));
    BEZEL_EXPECT_TRUE(strictlyDescending(alloca_seq));
}

// Reordering a capture frame must not change what the program computes: with
// v = ... no input here, the eight locals are 1..8 and their sum is 36 on
// both compilers. This pins that the fix is a determinism fix, not a
// correctness change.
BEZEL_TEST(capture_order_deterministic_3550, capture_frame_reorder_preserves_value) {
    auto r = runAot(kEightCapturedLocalsAcrossAnAwait);
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "DIAG: %s\n", d.message.c_str());
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 36);
}