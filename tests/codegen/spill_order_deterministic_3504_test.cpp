// fix #3504 — §29.3 / §30.2 (v0.283): the per-suspension spill/restore group
// must be emitted in a REPRODUCIBLE order.
//
// THE DEFECT. `emitAwaitSuspend` and the `yield` handler emitted their spill and
// restore groups by ITERATING `smAllocaToField_`, which was a
// `std::unordered_map<llvm::AllocaInst*, unsigned>`. Iterating a POINTER-keyed
// unordered_map orders by which key ADDRESSES collide modulo the bucket count,
// so any change to the compiler's own allocation order can silently reorder the
// emitted group. Measured in the issue: adding one unrelated
// `std::unordered_set::insert` on the compiler side reordered a program's
// spill/restore pairs, and that was the whole IR diff.
//
// It is not a miscompile — every member of a group is stored before the suspend
// branch and loaded after the resume, so the order within one group is free —
// but it costs two real things: `bezelc` stops being reproducible across
// allocator changes (ASan, a different libc, a heap-layout change), and it
// injects false DIFFs into the object-identity sweep that is this project's
// strongest evidence that a fix perturbs nothing it should not.
//
// WHY "COMPILE IT N TIMES AND COMPARE" IS NOT THE ORACLE. That test buys
// NOTHING here, and measuring it is what produced this one. libstdc++ puts a
// node into the global iteration list at the FRONT when its bucket is empty and
// beside its neighbour when it is not, so the order depends only on WHICH keys
// share a bucket — that is, on the pairwise DIFFERENCES of the addresses modulo
// a fixed prime, never on the base. ASLR shifts every alloca by one common
// offset, so it cannot change any of those differences. Measured, pre-fix: 40
// compiles of the issue's own repro in 40 separate processes produced 40
// byte-identical objects, and a standalone probe over 12 pointers printed the
// same iteration order on every run while the heap base moved by gigabytes. A
// determinism oracle built on repeated compilation would pass on the defect
// with probability 1.
//
// THE ORACLE USED INSTEAD is the emitted order itself. With the fix the group
// is emitted in FRAME FIELD order, so the field indices in one spill group read
// strictly ascending; pre-fix they read in the hash order, which for the program
// below is `10 9 8 6 7 5 4 3` — a permutation, not a sort. That is a property
// of the emitter, checkable on one compile, and it fails on the defect by
// construction rather than by luck.
//
// MEASURED on b43196a (pre-fix) vs the fix, same program:
//   spill group  : 10 9 8 6 7 5 4 3 12   ->   3 4 5 6 7 8 9 10 12
//   restore group: 10 9 8 6 7 5 4 3      ->   3 4 5 6 7 8 9 10
// and the program's value is 44 on both, which the last row pins: reordering a
// spill group must not change what the program computes.

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
// approach as static_init_atomic_1391_test / case_stmt_total_dead_edge_3073.
std::string bezelcPath() {
    std::array<char, 4096> buf{};
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return "";
    fs::path exe(std::string(buf.data(), static_cast<std::size_t>(n)));
    return (exe.parent_path().parent_path() / "bezelc").string();
}

// Compile to an object with BEZEL_DUMP_IR=1 and capture the module text the
// compiler prints to stderr (LlvmBackend.cpp). A SUBPROCESS, not in-process
// Driver: LLVM's errs() is a raw_fd_ostream whose capture by redirecting this
// process's own fd 2 was found unreliable in #1391.
//
// ⚠ CONSEQUENCE, shared with the other IR-dump tests (#1391, #3073): this row
// is an oracle on the `bezelc` BINARY, not on the `libbezel_core.a` this test
// links. If a build produces the test binary without relinking `bezelc` — which
// `scripts/agent_check.sh` does, since it builds `bezel_tests` and only reaches
// `bezelc` in its later lit phase — the row measures a STALE compiler and fails
// for a reason that has nothing to do with the source under test. Seen once
// while writing this test, and it happened to be a clean accidental
// FAIL-WITHOUT: against the parent `bezelc` the run count assertion passed (the
// groups were found) and `strictlyAscending` fired twice, once per group.
std::string compileAndCaptureIR(const std::string& source) {
    static int counter = 0;
    std::string bezelc = bezelcPath();
    if (bezelc.empty() || !fs::exists(bezelc)) return "";
    const char* envDir = std::getenv("BEZEL_AOT_SCRATCH_DIR");
    fs::path dir = envDir && *envDir ? fs::path(envDir)
                                     : fs::temp_directory_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string stem = "bezel_3504_" + std::to_string(getpid()) + "_" +
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
// The DEFINE anchor is load-bearing, not defensive: an async resume function is
// REFERENCED before it is defined (the entry function calls it, and
// `bz_task_then` takes its address), so a plain first-occurrence search for the
// mangled name lands in a different function's body and silently slices the
// wrong text. Measured while writing this test — the naive search produced a
// body with no frame GEPs at all, which reads exactly like "the fix removed
// them". Walk the occurrences and take the one whose line starts with `define`.
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

// One spill (or restore) of a suspend-local is exactly three instructions —
// a `getelementptr` on the frame, a `load`, and a `store` — so a whole group is
// a run of frame GEPs spaced exactly three lines apart. Returns each such run's
// FIELD INDICES, in emitted order, for runs of at least `minLen`.
std::vector<std::vector<unsigned>> frameGepRuns(const std::string& body,
                                                const std::string& frameTy,
                                                std::size_t minLen) {
    const std::string gepPrefix =
        "getelementptr inbounds nuw " + frameTy + ", ptr %1, i32 0, i32 ";
    std::vector<std::pair<unsigned, std::size_t>> hits;  // (field, line)
    std::size_t line = 0, pos = 0;
    while (pos <= body.size()) {
        std::size_t eol = body.find('\n', pos);
        std::string text = body.substr(
            pos, (eol == std::string::npos ? body.size() : eol) - pos);
        std::size_t at = text.find(gepPrefix);
        if (at != std::string::npos) {
            std::size_t numStart = at + gepPrefix.size();
            std::size_t numEnd = numStart;
            while (numEnd < text.size() && std::isdigit(
                       static_cast<unsigned char>(text[numEnd])))
                ++numEnd;
            if (numEnd > numStart)
                hits.emplace_back(
                    static_cast<unsigned>(
                        std::stoul(text.substr(numStart, numEnd - numStart))),
                    line);
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
        ++line;
    }
    std::vector<std::vector<unsigned>> runs;
    std::vector<unsigned> cur;
    std::size_t prevLine = 0;
    for (std::size_t i = 0; i < hits.size(); ++i) {
        if (!cur.empty() && hits[i].second != prevLine + 3) {
            if (cur.size() >= minLen) runs.push_back(cur);
            cur.clear();
        }
        cur.push_back(hits[i].first);
        prevLine = hits[i].second;
    }
    if (cur.size() >= minLen) runs.push_back(cur);
    return runs;
}

bool strictlyAscending(const std::vector<unsigned>& v) {
    for (std::size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] >= v[i]) return false;
    return true;
}

// Eight locals live across a single `await`, so the frame reserves eight
// consecutive spill fields and both the spill and the restore group are runs of
// eight. Eight is enough that a hash order and a sorted order cannot coincide
// by accident (1 in 8! = 40320 if the pre-fix order were a uniform permutation;
// it is not uniform, and it is measured NOT sorted on the parent compiler).
const char* kEightLocalsAcrossAnAwait =
    "module M; import System; class C "
    "  public static async method Step(v: int): Task<int> "
    "    var a := v + 1; var b := v + 2; var c := v + 3; var d := v + 4; "
    "    var e := v + 5; var f := v + 6; var g := v + 7; var h := v + 8; "
    "    await Task.Delay(1); "
    "    return a + b + c + d + e + f + g + h; "
    "  end Step; "
    "end C;";

}  // namespace

// The spill group and the restore group in the async resume function must each
// read in ascending FRAME FIELD order. Pre-fix both read `10 9 8 6 7 5 4 3`.
BEZEL_TEST(spill_order_deterministic_3504, await_spill_group_is_in_frame_order) {
    std::string ir = compileAndCaptureIR(kEightLocalsAcrossAnAwait);
    BEZEL_EXPECT_TRUE(!ir.empty());
    std::string body = sliceFunctionBody(ir, "bz$async$Step$Resume");
    BEZEL_EXPECT_TRUE(!body.empty());
    auto runs = frameGepRuns(body, "%async.Step", 8);
    // Two groups: the spill before the fast/slow branch and the restore at the
    // resume convergence. Asserting the COUNT is what stops this test from
    // passing vacuously if the emitted shape ever changes out from under it —
    // a silent "no runs found" would otherwise look identical to success.
    BEZEL_EXPECT_EQ(runs.size(), static_cast<std::size_t>(2));
    for (const auto& run : runs) {
        // >= 8, not == 8: the spill group is followed three lines later by the
        // pending-task field's own GEP, which the run detector picks up as a
        // ninth element. Harmless — it is a HIGHER field index than every
        // spill, so the ascending property is unaffected in both directions
        // (measured pre-fix: `10 9 8 6 7 5 4 3 12`, still not ascending).
        BEZEL_EXPECT_TRUE(run.size() >= 8);
        BEZEL_EXPECT_TRUE(strictlyAscending(run));
    }
}

// Reordering a spill group must not change what the program computes: with
// v = 1 the eight locals are 2..9 and their sum is 44 on both compilers.
BEZEL_TEST(spill_order_deterministic_3504, spill_group_reorder_preserves_value) {
    auto r = runAot(
        "module M; import System; class C "
        "  public static async method Step(v: int): Task<int> "
        "    var a := v + 1; var b := v + 2; var c := v + 3; var d := v + 4; "
        "    var e := v + 5; var f := v + 6; var g := v + 7; var h := v + 8; "
        "    await Task.Delay(1); "
        "    return a + b + c + d + e + f + g + h; "
        "  end Step; "
        "  public static async method Main(): Task<int> "
        "    var t := await C.Step(1); "
        "    return t; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}
