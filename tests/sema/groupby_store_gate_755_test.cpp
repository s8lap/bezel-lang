// partial #755 / stdlib §5: gate the crashing GroupBy "stored-in-a-variable"
// path with a clean diagnostic. The IGrouping<TKey,T> value model is not yet
// implemented; a BARE lazy `GroupBy(...)` result (IEnumerable<IGrouping<TKey,T>>)
// dangles when bound to a variable and SIGSEGVs on iteration (#755 [8]). Sema now
// rejects storing it with a clean message — a clean compile never crashes.
//
// The gate is precise: it matches ONLY the bare lazy enumerable of IGrouping
// (sema's makeLazyEnumerable(IGrouping<…>), a Named TypeRef with no astNode). A
// MATERIALIZED `.ToArray()` (Array<IGrouping>) or a terminal `.Count()`/`.Any()`
// is a different, working type and is NOT gated; the inline `for var g in
// <seq>.GroupBy(...)` form is a ForStmt (not a stored variable) and is untouched.
// These drive the real Driver (the diagnostic is in the front-end typecheck).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir755() {
    return wtScratchDir("wt-755");
}

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int "
           "using var a := new Array<int>(); "
           "a.Add(1); a.Add(2); a.Add(3); a.Add(4); " +
           body + " end Main; end C;";
}

int compileAndRun(const std::string& source) {
    auto dir = wtDir755();
    std::string stem = unique_stem("bezel_755");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

bool compileFails(const std::string& source) {
    auto dir = wtDir755();
    std::string stem = unique_stem("bezel_755_neg");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return !r.success;
}
}  // namespace

// #755 [8]: a BARE lazy GroupBy result stored in an inferred `var` then iterated
// used to compile cleanly and SIGSEGV at runtime. It is now a clean diagnostic.
BEZEL_TEST(groupby_store_gate_755, bare_groupby_stored_in_var_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(prog(
        "var n := 0; "
        "var groups := a.GroupBy(lambda(x: int) => x mod 2); "
        "for var g in groups do n := n + 1; end; "
        "return n;")));
}

// #755 [8]: the same store with an explicit `IEnumerable<IGrouping<…>>` type
// annotation is also rejected (the dangling lazy enumerator is the same).
BEZEL_TEST(groupby_store_gate_755, annotated_groupby_store_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(prog(
        "var groups: IEnumerable<IGrouping<int, int>> := "
        "  a.GroupBy(lambda(x: int) => x mod 2); "
        "return 0;")));
}

// #755 (no false positive): the inline `for var g in <seq>.GroupBy(...)` form is
// NOT a stored variable — it still compiles and counts the 2 groups (keys 0, 1).
BEZEL_TEST(groupby_store_gate_755, inline_for_over_groupby_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(prog(
        "var n := 0; "
        "for var g in a.GroupBy(lambda(x: int) => x mod 2) do n := n + 1; end; "
        "return n;")), 2);
}

// #755 (no false positive): a MATERIALIZED `.ToArray()` result (Array<IGrouping>,
// not the bare lazy enumerable) stored in a `using var` still works — 2 groups.
BEZEL_TEST(groupby_store_gate_755, groupby_toarray_store_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(prog(
        "using var groups := a.GroupBy(lambda(x: int) => x mod 2).ToArray(); "
        "return groups.Count;")), 2);
}

// #755 (no false positive): a terminal `.Count()` directly on a GroupBy result
// reduces to a scalar (not the bare lazy enumerable) and still works — 2 groups.
BEZEL_TEST(groupby_store_gate_755, groupby_count_terminal_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(prog(
        "return a.GroupBy(lambda(x: int) => x mod 2).Count();")), 2);
}

// #755 (no false positive): `.Any()` directly on a GroupBy result works — true.
BEZEL_TEST(groupby_store_gate_755, groupby_any_terminal_accepted) {
    BEZEL_EXPECT_EQ(compileAndRun(prog(
        "if a.GroupBy(lambda(x: int) => x mod 2).Any() then return 1; "
        "else return 0; end;")), 1);
}
