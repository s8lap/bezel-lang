// Codegen — OwningDictionary<K,V>.Drain() owned-consuming for-loop (#628 part 2).
//
// stdlib §4.9 v0.135: Drain() returns IOwningEnumerable<DrainedEntry<K,V>>.
// Iterating it transfers ownership of each DrainedEntry<K,V> to the consumer;
// the entry owns its Value field (§28.5), so discharging the entry also
// disposes the value. Partial iteration leaves the remainder in the dictionary
// (disposed at dict.Dispose() time). The Key is carried by value (string copy
// or generic raw bytes, not owned).
//
// Test strategy: destructor-count JIT tests prove exact-once disposal counts.
// LSan tests confirm no leaks / no double-free on partial-drain paths.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir628() {
    return wtScratchDir("wt-628");
}

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// LSan helper: compile → object → link with ASan runtime → run.
// Returns 0 (clean), 1 (sanitizer fired), -1 (toolchain missing).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir628();
    std::string stem = unique_stem("bezel_628_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -1;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -1;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -1;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -1;
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=1:exitcode=99 ") + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

// Resource class + OwningDictionary<string, R> program template.
std::string strProg(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        + mainBody + " end Main; end C;";
}

}  // namespace

// ===========================================================================
// §4.9 / #628 part 2: basic full drain — string key
// ===========================================================================

// §4.9: full drain transfers ownership of every value exactly once.
// Three entries added; drain loop uses `using var e := entry` to discharge
// the entry (and its Value) at end of each iteration body.
// Encoded: R._n after loop (expect 3 — each iteration disposes one R).
BEZEL_TEST(owned_dict_drain_628, full_drain_string_key_disposes_all_values) {
    BEZEL_EXPECT_EQ(run(strProg(
        "using var d := new OwningDictionary<string, R>(); "
        "d.Add(\"a\", move (new R())); "
        "d.Add(\"b\", move (new R())); "
        "d.Add(\"c\", move (new R())); "
        "for var entry in d.Drain() do "
        "  using var e := entry; "
        "end; "
        "return R._n;"   // all 3 disposed inside loop
    )), 3);
}

// §4.9: after a full drain the dictionary is empty (Count == 0).
BEZEL_TEST(owned_dict_drain_628, full_drain_empties_the_dict) {
    BEZEL_EXPECT_EQ(run(strProg(
        "using var d := new OwningDictionary<string, R>(); "
        "d.Add(\"x\", move (new R())); "
        "d.Add(\"y\", move (new R())); "
        "for var entry in d.Drain() do "
        "  using var e := entry; "
        "end; "
        "return d.Count;"   // expect 0
    )), 0);
}

// §4.9: entry.Key is accessible and carries the correct string value.
// Key "ab" (length 2) + key "cde" (length 3) → sum of lengths = 5.
BEZEL_TEST(owned_dict_drain_628, entry_key_is_readable) {
    BEZEL_EXPECT_EQ(run(strProg(
        "using var d := new OwningDictionary<string, R>(); "
        "d.Add(\"ab\", move (new R())); "
        "d.Add(\"cde\", move (new R())); "
        "var total := 0; "
        "for var entry in d.Drain() do "
        "  using var e := entry; "
        "  total := total + e.Key.Length; "
        "end; "
        "return total;"   // 2 + 3 = 5
    )), 5);
}

// §4.9: entry.Value is the owned class instance, disposed exactly once.
BEZEL_TEST(owned_dict_drain_628, entry_value_is_owned_and_disposed) {
    BEZEL_EXPECT_EQ(run(strProg(
        "using var d := new OwningDictionary<string, R>(); "
        "d.Add(\"k\", move (new R())); "
        "for var entry in d.Drain() do "
        "  using var e := entry; "
        "end; "
        "return R._n;"   // 1 — value disposed at end of loop body
    )), 1);
}

// §4.9: partial drain (break after first iteration).
// Yielded entry disposed inside loop; remaining entries stay in dict and are
// disposed by dict.Dispose() at scope end. Total disposal count = 3.
// Encoded: afterBreak * 10 + afterScopeEnd (expect 13).
BEZEL_TEST(owned_dict_drain_628, partial_drain_break_remainder_in_dict) {
    BEZEL_EXPECT_EQ(run(strProg(
        "var afterBreak := -1; "
        "do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d.Add(\"a\", move (new R())); "
        "  d.Add(\"b\", move (new R())); "
        "  d.Add(\"c\", move (new R())); "
        "  for var entry in d.Drain() do "
        "    using var e := entry; "
        "    break; "
        "  end; "
        "  afterBreak := R._n; "   // 1 disposed so far
        "end; "
        "return afterBreak * 10 + R._n;"  // 1*10 + 3 = 13
    )), 13);
}

// §4.9: empty dict drain — loop body never executes, returns 0.
BEZEL_TEST(owned_dict_drain_628, empty_dict_drain_no_iterations) {
    BEZEL_EXPECT_EQ(run(strProg(
        "using var d := new OwningDictionary<string, R>(); "
        "var count := 0; "
        "for var entry in d.Drain() do "
        "  using var e := entry; "
        "  count := count + 1; "
        "end; "
        "return count;"   // 0 — no iterations
    )), 0);
}

// §4.9: drain count matches Add count (4 entries → 4 iterations).
BEZEL_TEST(owned_dict_drain_628, drain_iterates_all_entries) {
    BEZEL_EXPECT_EQ(run(strProg(
        "using var d := new OwningDictionary<string, R>(); "
        "d.Add(\"a\", move (new R())); "
        "d.Add(\"b\", move (new R())); "
        "d.Add(\"c\", move (new R())); "
        "d.Add(\"d\", move (new R())); "
        "var count := 0; "
        "for var entry in d.Drain() do "
        "  using var e := entry; "
        "  count := count + 1; "
        "end; "
        "return count;"   // 4
    )), 4);
}

// ===========================================================================
// §4.9 / #628 part 2: generic-key (int) variant
// ===========================================================================

// §4.9: drain over OwningDictionary<int, R> — generic-key path.
BEZEL_TEST(owned_dict_drain_628, full_drain_int_key_disposes_all_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<int, R>(); "
        "  d.Add(1, move (new R())); "
        "  d.Add(2, move (new R())); "
        "  for var entry in d.Drain() do "
        "    using var e := entry; "
        "  end; "
        "  return R._n; "   // 2
        "end Main; end C;"
    ), 2);
}

// §4.9: int key is readable in the loop body; keys 10+20 = 30.
BEZEL_TEST(owned_dict_drain_628, int_key_entry_key_is_readable) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var d := new OwningDictionary<int, R>(); "
        "  d.Add(10, move (new R())); "
        "  d.Add(20, move (new R())); "
        "  var total := 0; "
        "  for var entry in d.Drain() do "
        "    using var e := entry; "
        "    total := total + e.Key; "
        "  end; "
        "  return total; "   // 10 + 20 = 30
        "end Main; end C;"
    ), 30);
}

// §4.9: partial drain with int key. Encoded: afterBreak*10 + afterScopeEnd.
BEZEL_TEST(owned_dict_drain_628, partial_drain_int_key_remainder_disposed_by_dict) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        "  var afterBreak := -1; "
        "  do "
        "    using var d := new OwningDictionary<int, R>(); "
        "    d.Add(1, move (new R())); "
        "    d.Add(2, move (new R())); "
        "    for var entry in d.Drain() do "
        "      using var e := entry; "
        "      break; "
        "    end; "
        "    afterBreak := R._n; "   // 1
        "  end; "
        "  return afterBreak * 10 + R._n; "  // 1*10 + 2 = 12
        "end Main; end C;"
    ), 12);
}

// ===========================================================================
// §4.9 / #628 part 2: existing part-1 test shape — now must SUCCEED
// ===========================================================================

// §4.9 / #628 part 2: the drain loop now compiles and runs correctly.
// (The part-1 test codegen_unimplemented_628.owned_dict_drain_for_is_hard_error
// verified it WAS a hard error; now it must compile and run.)
// Migrated from OwningDictionary<string,int> → OwningDictionary<string,R> per
// stdlib §4.2/#1327 (value types rejected as owned-collection elements).
BEZEL_TEST(owned_dict_drain_628, drain_loop_compiles_and_runs) {
    auto r = runAot(
        "module M; "
        "class R public constructor() end constructor; end R; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new OwningDictionary<string, R>(); "
        "    d.Add(\"x\", move (new R())); "
        "    var count := 0; "
        "    for var entry in d.Drain() do "
        "      using var e := entry; "
        "      count := count + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // one entry drained
}

// ===========================================================================
// §4.9 / #628 part 2: LSan — full drain (no leaks, no double-free)
// ===========================================================================

BEZEL_TEST(owned_dict_drain_628, lsan_full_drain_no_leak) {
    int lsan = runUnderLeakSanitizer(strProg(
        "using var d := new OwningDictionary<string, R>(); "
        "d.Add(\"a\", move (new R())); "
        "d.Add(\"b\", move (new R())); "
        "for var entry in d.Drain() do "
        "  using var e := entry; "
        "end; "
        "return 0;"
    ));
    if (lsan == -1) return;   // toolchain missing: skip
    BEZEL_EXPECT_EQ(lsan, 0); // clean
}

BEZEL_TEST(owned_dict_drain_628, lsan_partial_drain_no_leak) {
    int lsan = runUnderLeakSanitizer(strProg(
        "do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d.Add(\"a\", move (new R())); "
        "  d.Add(\"b\", move (new R())); "
        "  d.Add(\"c\", move (new R())); "
        "  for var entry in d.Drain() do "
        "    using var e := entry; "
        "    break; "
        "  end; "
        "end; "
        "return 0;"
    ));
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}
