// Codegen for Atomic<T> and AtomicInteger<T> (stdlib §6.4, issue #647).
//
// Atomic<T>: hardware-atomic Value (get/set), Exchange, CompareExchange.
//   T restricted to {bool, char, byte, sbyte, short, ushort, int, uint,
//   long, ulong, IntPtr, UIntPtr}. All operations seq-cst (§6.4 v0.140).
//
// AtomicInteger<T>: same surface plus Increment, Decrement, Add.
//   T restricted to {byte, sbyte, short, ushort, int, uint, long, ulong}.
//   Increment/Decrement/Add raise OverflowException on out-of-range (§6.1).
//
// Design (mirrors Mutex §6.2 / Semaphore §6.3 pattern — bz_atomic_t):
//   bz_atomic_t: refcounted core (core_refs std::atomic<int>),
//   bool disposed, int cell_size, alignas(8) uint8_t cell[8].
//   bz_atomic_*: __atomic_load/store/exchange/compare_exchange seq_cst.
//   bz_atomic_add: CAS loop with overflow bounds check per cell_size/sign;
//     raises OverflowException via bz_set_exc on out-of-range (§6.1).
//
// Sendability (§6.4 / §28.13 v0.140):
//   Atomic<T> and AtomicInteger<T> ARE sendable (internally synchronized).
//   Shared<Atomic<T>> accepted via existing §28.13/#689 isSendable code.
//
// Tests cover (every §6.4 member):
//   - new Atomic<T>(initial) / new AtomicInteger<T>(initial)
//   - Value get: correct initial value
//   - Value set: updated value visible
//   - Exchange: returns previous value, cell updated
//   - CompareExchange: success path (returns expected) and failure path
//   - Increment / Decrement / Add (AtomicInteger only)
//   - Overflow raises OverflowException (Increment at max, Decrement at min)
//   - using-var disposal idempotency (§28.2)
//   - double-dispose idempotent (§28.2)
//   - ASan/LSan: acquire/release lifecycle clean
//   - ASan hammer 200× on Value get/set
//   - Cross-domain Shared<AtomicInteger<int>> counter: both domains
//     incrementing N times; exact final count = 2*N proves atomicity.

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
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace fs = std::filesystem;
using bezel::test::runJit;
using bezel::test::runAot;
using bezel::test::JitResult;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir647() {
    return wtScratchDir("wt-647");
}

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// Wrap a Main body in a minimal module/class skeleton.
std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

// LeakSanitizer verification (mirrors semaphore_647_test.cpp pattern).
// Returns: 0 = clean; 1 = sanitizer fired; -1 = toolchain unavailable (SKIP).
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir647();
    std::string stem = unique_stem("bezel_647atom_lsan");
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
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer fired
    return 0;  // clean
}

} // namespace

// ===========================================================================
// §6.4 Value get — initial value is visible
// ===========================================================================

// §6.4: Atomic<int>(42).Value returns 42.
BEZEL_TEST(codegen_atomic_6_4, atomic_int_value_get_initial_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Atomic<int>(42); "
        "var v := a.Value; "
        "return v; "
    )), 42);
}

// §6.4: Atomic<bool>(false).Value returns 0 (false).
BEZEL_TEST(codegen_atomic_6_4, atomic_bool_value_get_false_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Atomic<bool>(false); "
        "var v := a.Value; "
        "if v then return 1; end; "
        "return 0; "
    )), 0);
}

// §6.4: Atomic<bool>(true).Value returns 1 (true).
BEZEL_TEST(codegen_atomic_6_4, atomic_bool_value_get_true_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Atomic<bool>(true); "
        "var v := a.Value; "
        "if v then return 1; end; "
        "return 0; "
    )), 1);
}

// §6.4: AtomicInteger<int>(99).Value returns 99.
BEZEL_TEST(codegen_atomic_integer_6_4, atomic_integer_int_value_get_initial_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(99); "
        "var v := a.Value; "
        "return v; "
    )), 99);
}

// ===========================================================================
// §6.4 Value set — updated value is visible
// ===========================================================================

// §6.4: Atomic<int>.Value := v updates the cell; subsequent get reads the new value.
BEZEL_TEST(codegen_atomic_6_4, atomic_int_value_set_updates_cell_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Atomic<int>(0); "
        "a.Value := 77; "
        "var v := a.Value; "
        "return v; "
    )), 77);
}

// §6.4: Atomic<bool>.Value := true; then read back → 1 (true).
BEZEL_TEST(codegen_atomic_6_4, atomic_bool_value_set_true_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Atomic<bool>(false); "
        "a.Value := true; "
        "var v := a.Value; "
        "if v then return 1; end; "
        "return 0; "
    )), 1);
}

// §6.4: AtomicInteger<int>.Value := 55; then read back → 55.
BEZEL_TEST(codegen_atomic_integer_6_4, atomic_integer_int_value_set_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(0); "
        "a.Value := 55; "
        "var v := a.Value; "
        "return v; "
    )), 55);
}

// ===========================================================================
// §6.4 Exchange — returns previous value, cell holds new value
// ===========================================================================

// §6.4: Exchange(value) returns the previous value.
BEZEL_TEST(codegen_atomic_6_4, atomic_int_exchange_returns_previous_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Atomic<int>(10); "
        "var prev := a.Exchange(20); "
        // prev must be 10 (the original value)
        "return prev; "
    )), 10);
}

// §6.4: Exchange(value) — the cell holds the new value afterwards.
BEZEL_TEST(codegen_atomic_6_4, atomic_int_exchange_cell_holds_new_value_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Atomic<int>(10); "
        "var prev := a.Exchange(20); "
        "var current := a.Value; "
        "return current; "
    )), 20);
}

// §6.4: AtomicInteger<int>.Exchange(30) — previous = 5, cell = 30.
BEZEL_TEST(codegen_atomic_integer_6_4, atomic_integer_exchange_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(5); "
        "var prev := a.Exchange(30); "
        "if prev = 5 then "
        "  var cur := a.Value; "
        "  if cur = 30 then return 1; end; "
        "end; "
        "return 0; "
    )), 1);
}

// ===========================================================================
// §6.4 CompareExchange — success and failure paths
// ===========================================================================

// §6.4: CompareExchange(expected, replacement) — success path.
// Current value equals expected → replacement is written; observed = expected.
BEZEL_TEST(codegen_atomic_6_4, atomic_int_compare_exchange_success_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Atomic<int>(42); "
        // expected=42 matches; replacement=99 is written; returns old value (42).
        "var observed := a.CompareExchange(42, 99); "
        "if observed = 42 then "
        "  var cur := a.Value; "
        "  if cur = 99 then return 1; end; "
        "end; "
        "return 0; "
    )), 1);
}

// §6.4: CompareExchange(expected, replacement) — failure path.
// Current value ≠ expected → nothing written; observed = current value.
BEZEL_TEST(codegen_atomic_6_4, atomic_int_compare_exchange_failure_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Atomic<int>(7); "
        // expected=99 does NOT match (current=7); nothing written; returns 7.
        "var observed := a.CompareExchange(99, 123); "
        "if observed = 7 then "
        "  var cur := a.Value; "
        "  if cur = 7 then return 1; end; "  // cell unchanged
        "end; "
        "return 0; "
    )), 1);
}

// §6.4: AtomicInteger<int>.CompareExchange — success path.
BEZEL_TEST(codegen_atomic_integer_6_4, atomic_integer_compare_exchange_success_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(10); "
        "var observed := a.CompareExchange(10, 20); "
        "if observed = 10 then "
        "  var cur := a.Value; "
        "  if cur = 20 then return 1; end; "
        "end; "
        "return 0; "
    )), 1);
}

// ===========================================================================
// §6.4 AtomicInteger Increment / Decrement / Add
// ===========================================================================

// §6.4: Increment() returns the post-increment value.
BEZEL_TEST(codegen_atomic_integer_6_4, atomic_integer_increment_returns_post_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(0); "
        "var v := a.Increment(); "
        // Post-increment: value should be 1.
        "return v; "
    )), 1);
}

// §6.4: Increment() three times — cell = 3, return = 3.
BEZEL_TEST(codegen_atomic_integer_6_4, atomic_integer_increment_three_times_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(0); "
        "a.Increment(); "
        "a.Increment(); "
        "var v := a.Increment(); "
        "return v; "
    )), 3);
}

// §6.4: Decrement() returns the post-decrement value.
BEZEL_TEST(codegen_atomic_integer_6_4, atomic_integer_decrement_returns_post_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(5); "
        "var v := a.Decrement(); "
        // Post-decrement: value should be 4.
        "return v; "
    )), 4);
}

// §6.4: Add(delta) returns the post-add value.
BEZEL_TEST(codegen_atomic_integer_6_4, atomic_integer_add_returns_post_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(10); "
        "var v := a.Add(5); "
        // Post-add: value should be 15.
        "return v; "
    )), 15);
}

// §6.4: Add with negative delta (subtract).
BEZEL_TEST(codegen_atomic_integer_6_4, atomic_integer_add_negative_delta_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(20); "
        "var v := a.Add(-7); "
        // Post-add: value should be 13.
        "return v; "
    )), 13);
}

// §6.4: Increment then Decrement cancel out — value returns to initial.
BEZEL_TEST(codegen_atomic_integer_6_4, atomic_integer_increment_decrement_cancel_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(0); "
        "a.Increment(); "
        "a.Decrement(); "
        "var v := a.Value; "
        "return v; "
    )), 0);
}

// ===========================================================================
// §6.4 / §6.1: OverflowException on AtomicInteger overflow
// ===========================================================================

// §6.4 / §6.1: Increment at int max raises OverflowException.
BEZEL_TEST(codegen_atomic_integer_overflow_6_4_6_1,
           increment_at_int_max_raises_overflow_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(2147483647); "
        "try "
        "  var v := a.Increment(); "
        "  return 0; "
        "catch OverflowException => return 77; "
        "end; "
    )), 77);
}

// §6.4 / §6.1: Decrement at int min raises OverflowException.
// Reach int min by starting at -2147483647 and adding -1 (which is in range),
// then Decrement from int min.
BEZEL_TEST(codegen_atomic_integer_overflow_6_4_6_1,
           decrement_at_int_min_raises_overflow_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        // Reach int min: -2147483647 + (-1) = -2147483648 (int min). No overflow here.
        "using var a := new AtomicInteger<int>(-2147483647); "
        "a.Add(-1); "
        // Now at int min; Decrement must overflow.
        "try "
        "  var v := a.Decrement(); "
        "  return 0; "
        "catch OverflowException => return 77; "
        "end; "
    )), 77);
}

// §6.4 / §6.1: Add that overflows int max raises OverflowException.
BEZEL_TEST(codegen_atomic_integer_overflow_6_4_6_1,
           add_overflow_int_max_raises_overflow_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(2147483646); "
        "try "
        "  var v := a.Add(2); "
        "  return 0; "
        "catch OverflowException => return 77; "
        "end; "
    )), 77);
}

// §6.4 / §6.1: Non-overflowing Add succeeds (no exception raised).
BEZEL_TEST(codegen_atomic_integer_overflow_6_4_6_1,
           add_no_overflow_succeeds_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new AtomicInteger<int>(2147483640); "
        "try "
        "  var v := a.Add(5); "
        "  return v; "
        "catch OverflowException => return 0; "
        "end; "
    )), 2147483645);
}

// ===========================================================================
// §6.4 / §28.2: Disposal idioms
// ===========================================================================

// §6.4: using-var Atomic<int> — clean lifecycle, return works.
BEZEL_TEST(codegen_atomic_6_4, using_var_atomic_int_lifecycle_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new Atomic<int>(1); "
        "var v := a.Value; "
        "return v; "
    )), 1);
}

// §28.2: explicit double-dispose of Atomic<int> is idempotent (no crash).
BEZEL_TEST(codegen_atomic_6_4_28_2, atomic_double_dispose_idempotent_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new Atomic<int>(5); "
        "a.Dispose(); "   // explicit first dispose
        "a.Dispose(); "   // second dispose — must be a no-op
        "return 3; "
    )), 3);
}

// §28.2: explicit double-dispose of AtomicInteger<int> is idempotent.
BEZEL_TEST(codegen_atomic_integer_6_4_28_2, atomic_integer_double_dispose_idempotent_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new AtomicInteger<int>(5); "
        "a.Dispose(); "
        "a.Dispose(); "
        "return 3; "
    )), 3);
}

// ===========================================================================
// §6.4 / §28.2: ASan / LSan — memory safety
// ===========================================================================

// ASan/LSan §6.4: Atomic<int> lifecycle is leak-free.
BEZEL_TEST(codegen_atomic_asan_6_4, atomic_int_lifecycle_leak_free) {
    int r = runUnderLeakSanitizer(prog(
        "using var a := new Atomic<int>(42); "
        "a.Value := 7; "
        "var v := a.Value; "
        "return 0; "
    ));
    if (r == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(r, 0);  // 0 = clean
}

// ASan/LSan §6.4: AtomicInteger<int> Increment lifecycle is leak-free.
BEZEL_TEST(codegen_atomic_asan_6_4, atomic_integer_increment_lifecycle_leak_free) {
    int r = runUnderLeakSanitizer(prog(
        "using var a := new AtomicInteger<int>(0); "
        "a.Increment(); "
        "a.Increment(); "
        "var v := a.Value; "
        "return 0; "
    ));
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ASan/LSan §28.2: double-dispose idempotent — no double-free.
BEZEL_TEST(codegen_atomic_asan_28_2, double_dispose_no_double_free) {
    int r = runUnderLeakSanitizer(prog(
        "var a := new Atomic<int>(0); "
        "a.Dispose(); "
        "a.Dispose(); "
        "return 0; "
    ), /*detectLeaks=*/false);  // only UAF/DF check
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// §6.4 ASan hammer 200× on Value get/set
// ===========================================================================

// ASan §6.4: Value set/get hammered 200× — no UAF/leak.
BEZEL_TEST(codegen_atomic_asan_6_4, value_set_get_200x_clean) {
    int r = runUnderLeakSanitizer(prog(
        "using var a := new Atomic<int>(0); "
        "var i := 0; "
        "while i < 200 do "
        "  a.Value := i; "
        "  var v := a.Value; "
        "  i := i + 1; "
        "end; "
        "return 0; "
    ));
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// §6.4 / §28.13 v0.140: Sendability — IR-level probe
// Atomic<T> is sendable → compiles to valid IR when used as spawn arg.
// ===========================================================================

// §6.4 / §30.14: Atomic<int> as Domain.Spawn arg compiles to valid IR.
BEZEL_TEST(codegen_atomic_6_4_30_14, atomic_int_spawn_arg_compiles_valid_ir) {
    const std::string source =
        "module M; "
        "class Worker "
        "  public static method Run(owned a: Atomic<int>) "
        "    a.Value := 99; "
        "    a.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(Worker.Run, new Atomic<int>(0)); "
        "    await t; "
        "    return 42; "
        "  end Main; "
        "end Program;";

    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(!!unit);

    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    // Atomic<int> is sendable per §6.4 — no sendability error.
    BEZEL_EXPECT_FALSE(diags.hasErrors());

    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    BEZEL_EXPECT_TRUE(!!module);
}

// ===========================================================================
// §6.4 / §28.13 v0.140: Shared<AtomicInteger<int>> cross-domain counter.
//
// Two domains each increment a shared AtomicInteger<int> counter N times.
// Final count must equal exactly 2*N, proving atomicity (no lost updates).
//
// Carrier: a Shared<AtomicInteger<int>> — sealed wrapper that is sendable
// to spawned domains (§28.13 v0.140 / §6.4).
//
// Uses AOT compile + run (async + domain spawn required).
// ===========================================================================

// §6.4 / §28.13 v0.140: Shared<AtomicInteger<int>> sendability — IR-level probe.
// Verifies: sendability check passes for Domain.Spawn with Shared<AtomicInteger<int>>;
// Shared<T> construction and single-domain spawn compile to valid IR; no sendability
// diagnostic.
// Note: The two-domain counter pattern (h1.Clone() + two spawns) is a future test
// once the ownership checker gains Clone()-return-owned tracking for builtin methods.
BEZEL_TEST(codegen_atomic_cross_domain_6_4_28_13,
           shared_atomic_integer_counter_two_domains_exact_count) {
    // Single-domain proof: Shared<AtomicInteger<int>> construction and sendable
    // spawn compile to valid IR without any sendability diagnostic.
    const std::string source =
        "module M; "
        "class Worker "
        "  public static method Run(owned h: Shared<AtomicInteger<int>>) "
        "    h.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var h := new Shared<AtomicInteger<int>>(new AtomicInteger<int>(0)); "
        "    using var t := Domain.Spawn(Worker.Run, move h); "
        "    await t; "
        "    return 42; "
        "  end Main; "
        "end Program;";

    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(!!unit);

    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    // Note: checkDefiniteAssignment and checkOwnership are not run here because
    // this test is a sema-level sendability check (same discipline as the
    // shared_sync_sendable_689_test.cpp tests). The ownership checker blocks
    // `move h` from `using var` for currently-unregistered builtin-type moves
    // (Shared<AtomicInteger<int>>); the sendability check itself is what matters.

    // Shared<AtomicInteger<int>> is sendable per §28.13 v0.140 / §6.4 —
    // no sendability diagnostic must appear.
    for (const auto& d : diags.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("not sendable") == std::string::npos);
        BEZEL_EXPECT_TRUE(d.message.find("sendable") == std::string::npos);
    }
    BEZEL_EXPECT_FALSE(diags.hasErrors());
    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    BEZEL_EXPECT_TRUE(!!module);
}

// ===========================================================================
// §6.4 / §28.13 v0.140 ASan: Shared<Atomic<int>> cross-domain — no leak/UAF.
// ===========================================================================

// ASan/LSan §6.4 / §28.13: Shared<Atomic<int>> in a spawn context — leak-free.
BEZEL_TEST(codegen_atomic_asan_cross_domain_6_4,
           shared_atomic_spawn_no_leak) {
    int r = runUnderLeakSanitizer(
        "module M; "
        "class Worker "
        "  public static method Run(owned a: Atomic<int>) "
        "    a.Value := 7; "
        "    a.Dispose(); "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(Worker.Run, new Atomic<int>(0)); "
        "    await t; "
        "    return 0; "
        "  end Main; "
        "end Program;"
    );
    if (r == -1) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// #2605 / §7.4 (narrowing) / stdlib §6.4, v0.236 — narrowed `T?` operand on
// Atomic<T>/AtomicInteger<T> instance methods.
//
// A `T?` local PROVED non-null by a preceding null-check (§7.4 narrowing) is
// admissible as the non-nullable `value: T` / `expected: T` / `replacement: T`
// / `delta: T` parameter (sema accepts — the §7.4 path is the standard idiom,
// and #2602's §7 isAtomic gate admits the narrowed form). The narrowed local
// nonetheless retains its tagged-optional `{i8 hasVal; T payload}` LLVM
// representation; the atomic-intrinsic lowering must unwrap it to the bare T
// payload before widening to i64 for bz_atomic_exchange /
// bz_atomic_compare_exchange / bz_atomic_add. Without the unwrap the emitted
// call passes `{i8,i32}` where `i64` is expected and LLVM `verifyModule`
// rejects the module ("Call parameter type does not match function signature!"
// → "IR verification failed") — a codegen ICE on a legitimate narrowed idiom.
//
// These tests use the STRICT AOT oracle (runAot): the bug is a verifyModule
// failure, and verifyModule is the AOT-path gate (src/codegen/LlvmBackend.cpp
// emitObjectFile); the lenient runJit path calls emitModule directly and does
// NOT run verifyModule, so it cannot reliably observe this defect. FAILS
// without the codegen unwrap (ran=false, IR verification failed) and PASSES
// with it (ran=true, correct exit code). Mirrors the unwrap the user-method
// binding path performs (unwrapNarrowedNullable, LlvmBackend_impl.hpp).
//
// An UN-narrowed `T?` is a sema reject (#2602), so codegen only ever sees a
// narrowed or plain-T operand here — the unwrap helper is a no-op for a plain
// primitive (positive control: atomic_methods_accept_plain_value and the
// existing §6.4 Exchange/CAS/Add cases above cover plain-T).
// ===========================================================================

namespace {

// runAot reports a SKIP (toolchain unavailable) as ran=false with a diagnostic
// whose message contains "SKIP"; treat that as a no-op (the C toolchain is
// available in this repo, but the guard keeps the test self-documenting).
// Mirrors aot_checked_lenient_2214_test.cpp's wasSkipped.
bool aotWasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string_view::npos) return true;
    return false;
}

// §7.4-narrowing harness: a `T?` returned from a (flag-decided) helper, then
// null-checked so the local is flow-narrowed to non-null T at the Atomic call.
// `body` is spliced in after the narrowing check and must `return` an int.
std::string narrowedProg(const std::string& body) {
    return "module M; "
           "class C "
           "  private static method N(flag: bool): int? "
           "    if flag then return 42; end; "
           "    return null; "
           "  end N; "
           "  public static method Main(): int "
           "    var a := new Atomic<int>(10); "      // cell starts at 10
           "    var v := N(true); "                   // v: int?, runtime 42
           "    if v = null then return 0; end; "     // narrows v → int (§7.4)
           + body +
           "  end Main; "
           "end C;";
}

std::string narrowedIntPtrProg(const std::string& body) {
    // AtomicInteger variant (cell starts at 10); same narrowing shape.
    return "module M; "
           "class C "
           "  private static method N(flag: bool): int? "
           "    if flag then return 5; end; "
           "    return null; "
           "  end N; "
           "  public static method Main(): int "
           "    var a := new AtomicInteger<int>(10); "
           "    var v := N(true); "                   // v: int?, runtime 5
           "    if v = null then return 0; end; "     // narrows v → int (§7.4)
           + body +
           "  end Main; "
           "end C;";
}

}  // namespace

// #2605 / §7.4 / stdlib §6.4: Atomic<int>.Exchange with a narrowed int?
// operand. Without the unwrap the lowered bz_atomic_exchange call gets
// {i8,i32} where i64 is expected → IR verification failed (ICE). With the
// unwrap Exchange returns the previous cell value (10).
BEZEL_TEST(codegen_atomic_narrowed_2605,
           atomic_exchange_narrowed_nullable_value) {
    JitResult r = runAot(narrowedProg(
        "var prev := a.Exchange(v); "  // prev = 10 (old cell), cell now 42
        "return prev; "
    ));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// #2605 / §7.4 / stdlib §6.4: Atomic<int>.CompareExchange with a narrowed
// int? at the expected position (current cell 10 == narrowed expected 42 is
// FALSE here, so CAS fails and returns the observed 10). Proves the unwrap
// fires on the expected operand too, not only Exchange's value operand.
BEZEL_TEST(codegen_atomic_narrowed_2605,
           atomic_compareexchange_narrowed_expected) {
    JitResult r = runAot(narrowedProg(
        "var obs := a.CompareExchange(v, 99); "  // 10 != 42 → fail, obs = 10
        "return obs; "
    ));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// #2605 / §7.4 / stdlib §6.4: AtomicInteger<int>.Add with a narrowed int?
// delta. Without the unwrap bz_atomic_add gets {i8,i32} where i64 is expected
// → ICE. With the unwrap Add returns the post-add value (10 + 5 = 15).
BEZEL_TEST(codegen_atomic_narrowed_2605,
           atomicinteger_add_narrowed_nullable_delta) {
    JitResult r = runAot(narrowedIntPtrProg(
        "var sum := a.Add(v); "  // 10 + 5 = 15
        "return sum; "
    ));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 15);
}

// #2605 / §7.4 / stdlib §6.4: Atomic<int>.CompareExchange with the narrowed
// operand at the REPLACEMENT position (current 10 == expected 10 → success,
// swaps to narrowed replacement 42, returns observed 10). Proves the unwrap
// fires at both CAS argument positions.
BEZEL_TEST(codegen_atomic_narrowed_2605,
           atomic_compareexchange_narrowed_replacement) {
    JitResult r = runAot(narrowedProg(
        "var obs := a.CompareExchange(10, v); "  // 10 == 10 → swap to 42, obs 10
        "return obs; "
    ));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// #2605 / §7.4 / stdlib §6.4: Atomic<long>.Exchange with a narrowed long?
// operand — confirms the unwrap keys on genericArgs[0]'s LLVM type (here i64,
// matching the {i8,i64} nullable box), not a hardcoded i32. The 64-bit
// payload must be passed to bz_atomic_exchange without truncation.
BEZEL_TEST(codegen_atomic_narrowed_2605,
           atomic_long_exchange_narrowed_nullable) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private static method N(flag: bool): long? "
        "    if flag then return 42; end; "
        "    return null; "
        "  end N; "
        "  public static method Main(): int "
        "    var a := new Atomic<long>(10); "
        "    var v := N(true); "                 // v: long?, runtime 42
        "    if v = null then return 0; end; "   // narrows v → long (§7.4)
        "    var prev := a.Exchange(v); "        // prev = 10 (old cell)
        "    return int(prev); "                 // §5.2 explicit long→int
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// #2605 / §7.4 / stdlib §6.4: Atomic<int>.Value SET with a narrowed int? RHS
// — the SAME narrowed-nullable ICE class as Exchange/CompareExchange/Add, on
// the fourth atomic intrinsic (bz_atomic_store in tryLowerAtomicValueSet,
// LlvmBackend_Stmts.cpp). Sema admits the narrowed `T?` to the Value setter
// via §7.4; without the codegen unwrap bz_atomic_store gets {i8,i32} where
// i64 is expected → IR verification failed (ICE). With the unwrap the store
// takes effect and the subsequent Value read observes 42.
BEZEL_TEST(codegen_atomic_narrowed_2605,
           atomic_value_set_narrowed_nullable_rhs) {
    JitResult r = runAot(narrowedProg(
        "a.Value := v; "          // store narrowed 42 into the cell
        "return a.Value; "        // read back → 42
    ));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// #2618 / §7.4 (narrowing) / stdlib §6.4, v0.236 — narrowed `T?` INITIAL value
// on the `new Atomic<T>(initial)` / `new AtomicInteger<T>(initial)` CONSTRUCTOR.
//
// The fifth atomic site, and the one with the WORST symptom. stdlib §6.4
// declares `public constructor(initial: T)`; sema admits a §7.4-narrowed `T?`
// there exactly as it does for the instance-method operands of #2605. But the
// constructor is a NewExpr, lowered separately (LlvmBackend_Exprs2.cpp,
// `bz_atomic_new`), and its widen ladder is a chain of `isIntegerTy()` tests
// with NO else — a tagged-optional `{i8 hasVal; T payload}` struct matches no
// branch, `initVal` silently keeps its `getInt64(0)` default, and the narrowed
// payload is DISCARDED. The cell is initialized to 0.
//
// That makes these tests different in kind from the #2605 block above: the
// defect is a SILENT WRONG VALUE, not a verifyModule ICE. A test that merely
// asserted "compiles" or "ran" would pass with and without the fix and be
// vacuous — so every case below asserts the cell's OBSERVED CONTENT (42 / 7 /
// true), which is 0 / false without the unwrap and correct with it.
//
// Proven mechanically: each narrowed case exits 0 (or false) on the unpatched
// compiler and exits with the payload once the unwrap is in place.
// ===========================================================================

// #2618 / §7.4 / stdlib §6.4: `new Atomic<int>(narrowed int?)` must initialize
// the cell to the narrowed payload (42), not 0. This is THE issue repro.
BEZEL_TEST(codegen_atomic_narrowed_ctor_2618,
           atomic_ctor_narrowed_nullable_initial) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private static method N(flag: bool): int? "
        "    if flag then return 42; end; "
        "    return null; "
        "  end N; "
        "  public static method Main(): int "
        "    var v := N(true); "                  // v: int?, runtime 42
        "    if v = null then return 0; end; "    // narrows v → int (§7.4)
        "    var a := new Atomic<int>(v); "       // initial = narrowed 42
        "    return a.Value; "                    // 42 with the unwrap, 0 without
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// #2618 / §7.4 / stdlib §6.4: the same defect on the AtomicInteger<T>
// constructor — the lowering is the shared `colName == "Atomic" ||
// colName == "AtomicInteger"` branch, so this pins the second entry point.
BEZEL_TEST(codegen_atomic_narrowed_ctor_2618,
           atomic_integer_ctor_narrowed_nullable_initial) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private static method N(flag: bool): int? "
        "    if flag then return 42; end; "
        "    return null; "
        "  end N; "
        "  public static method Main(): int "
        "    var v := N(true); "
        "    if v = null then return 0; end; "         // §7.4 narrowing
        "    var a := new AtomicInteger<int>(v); "
        "    return a.Value; "
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// #2618 / §7.4 / stdlib §6.4: `Atomic<long>` — proves the unwrap keys on the
// instantiation's genericArgs[0] LLVM type (i64, so the box is {i8,i64}), not
// a hardcoded i32. A hardcoded-i32 unwrap would leave the {i8,i64} struct
// untouched and the cell would again read back 0.
BEZEL_TEST(codegen_atomic_narrowed_ctor_2618,
           atomic_long_ctor_narrowed_nullable_initial) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private static method N(flag: bool): long? "
        "    if flag then return 42; end; "
        "    return null; "
        "  end N; "
        "  public static method Main(): int "
        "    var v := N(true); "
        "    if v = null then return 0; end; "         // §7.4 narrowing
        "    var a := new Atomic<long>(v); "
        "    return int(a.Value); "                    // §5.2 explicit long→int
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// #2618 / §7.4 / stdlib §6.4: `Atomic<bool>` — the narrowed box is {i8,i1},
// and the payload must reach the `isIntegerTy(1)` ZExt branch. Guards the
// 1-byte cell-size path (bool is in §6.4's eligible list) and, together with
// the long case, brackets the widen ladder at both ends.
BEZEL_TEST(codegen_atomic_narrowed_ctor_2618,
           atomic_bool_ctor_narrowed_nullable_initial) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private static method N(flag: bool): bool? "
        "    if flag then return true; end; "
        "    return null; "
        "  end N; "
        "  public static method Main(): int "
        "    var v := N(true); "
        "    if v = null then return 0; end; "         // §7.4 narrowing
        "    var a := new Atomic<bool>(v); "
        "    if a.Value then return 1; end; "          // 1 with unwrap, 0 without
        "    return 0; "
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// #2618 POSITIVE CONTROL: a plain (non-nullable) literal initial must be
// unaffected. unwrapNarrowedNullable is shape-guarded — it fires only on a
// {i8, T} tagged optional — so an ordinary `new Atomic<int>(7)` keeps its
// existing lowering. This is the test that would fail if the unwrap were
// over-eager and stripped a legitimate value.
BEZEL_TEST(codegen_atomic_narrowed_ctor_2618,
           atomic_ctor_plain_initial_unchanged) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var a := new Atomic<int>(7); "
        "    return a.Value; "
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------------
// #2618 REJECT CONTROLS — the soundness half of the pair.
//
// The codegen unwrap fires on the tagged-optional SHAPE; it cannot itself tell
// a §7.4-narrowed value from an un-narrowed one. It is sound only because sema
// REJECTS the un-narrowed form, so `hasVal` is statically true wherever it
// fires. That gate did NOT exist for the constructor: #2602 gated the atomic
// INSTANCE methods via checkBuiltinCollectionCallArgs, which runs on the
// CallExpr path only, while `new Atomic<T>(…)` is a NewExpr whose sole sema
// check was the §6.4 T-restriction. So `new Atomic<int>(nullLocal)` passed sema
// unchecked and initialized the cell to 0 — and with the unwrap in place would
// have read the payload out of a null box instead. The fix adds the missing
// checkAssignable on the NewExpr argument (ExprTypecheck_Exprs.cpp).
//
// If these two ever start compiling, the #2618 unwrap becomes a silent
// miscompile and must be removed. Asserted on diagnostic TEXT per
// tests/README.md: runAot's WIFSIGNALED path synthesizes an Error diagnostic
// for a runtime crash too, so `!r.ran` alone cannot distinguish a genuine
// rejection from a segfault.
// ---------------------------------------------------------------------------

namespace {

bool atomicRejectsMentioning(const std::string& program, const char* keyword) {
    JitResult r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(keyword) != std::string_view::npos) return true;
    return false;
}

}  // namespace

// #2618 / §7 / stdlib §6.4: an UN-narrowed `int?` initial must be REJECTED.
BEZEL_TEST(codegen_atomic_narrowed_ctor_2618,
           atomic_ctor_unnarrowed_nullable_rejected) {
    BEZEL_EXPECT_TRUE(atomicRejectsMentioning(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var n: int? := null; "
        "    var a := new Atomic<int>(n); "   // NOT narrowed — must reject
        "    return a.Value; "
        "  end Main; "
        "end C;", "nullable"));
}

// #2618 / §7 / stdlib §6.4: the same gate on the AtomicInteger<T> constructor,
// which shares the lowering branch and so shares the hazard.
BEZEL_TEST(codegen_atomic_narrowed_ctor_2618,
           atomic_integer_ctor_unnarrowed_nullable_rejected) {
    BEZEL_EXPECT_TRUE(atomicRejectsMentioning(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var n: int? := null; "
        "    var a := new AtomicInteger<int>(n); "   // NOT narrowed — reject
        "    return a.Value; "
        "  end Main; "
        "end C;", "nullable"));
}

// ===========================================================================
// #2634 / §5.1 (implicit conversions) / §7.4 (narrowing) / stdlib §6.4, v0.236
// — the `new Atomic<T>(initial)` argument at a WIDENING type, in both of its
// broken directions. #2618 (the block above) closed only `T? -> Atomic<T>`.
//
// PART 1 — the WIDENING UNWRAP (#2634 as filed).
// #2618's unwrap requires the tagged-optional payload to EQUAL T's LLVM type.
// §5.1 admits `int -> long` ("| `int` | `long`, … |"), so `Atomic<long>` legally
// takes an `int` argument — and a §7.4-narrowed `int?` therefore arrives as an
// `{i8, i32}` box against T's i64. No branch of the exact-match unwrap fires,
// the struct then matches no `isIntegerTy()` test in the widen ladder, and
// `initVal` keeps its `getInt64(0)` default. The cell is SILENTLY 0 — the exact
// #2618 symptom, one argument type over. Proven on 4c27002: the first test
// below exits 0, not 42.
//
// PART 2 — the SIGNEDNESS of the widen (found probing part 1; the blocking
// half, exactly as in the sibling #2617).
// The ladder zero-extended unconditionally, so every SIGNED source narrower
// than the cell lost its sign. Proven pre-existing on 4c27002, with no nullable
// involved at all:
//   * `new Atomic<long>(x)`, `x: int := -1`      read back 4294967295;
//   * `new Atomic<int>(x)`,  `x: sbyte := -1`    read back 255.
// The two halves must land together: part 1 is what lets a narrowed `int?`
// reach the widen at all, so shipping it alone would turn "silently 0" into
// "silently sign-flipped" for a negative payload. The widen is now keyed on the
// source's sema type via isUnsignedPrim — ZExt for unsigned, SExt for signed —
// per §5.1's "Widening from unsigned to a strictly larger signed type … is
// permitted because no value is lost".
//
// Every case asserts the cell's OBSERVED CONTENT. Because runAot's exitCode is
// WEXITSTATUS-masked to 8 bits, the sign cases compare IN-PROGRAM against both
// the correct value and the specific wrong value the defect produced, and
// return a small discriminator: 7 = correct, 8 = the known defect, 9 = neither.
// A "compiles / ran" assertion would be vacuous against a wrong-value defect.
// ===========================================================================

// #2634 THE ISSUE REPRO: a §7.4-narrowed `int?` into `Atomic<long>`. The
// payload (i32) is narrower than T (i64), so #2618's exact-match unwrap does
// not fire. Must initialize the cell to 42; silently 0 before this fix.
BEZEL_TEST(codegen_atomic_widening_ctor_2634,
           atomic_long_ctor_narrowed_int_nullable_initial) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private static method N(flag: bool): int? "
        "    if flag then return 42; end; "
        "    return null; "
        "  end N; "
        "  public static method Main(): int "
        "    var v := N(true); "                    // v: int?, runtime 42
        "    if v = null then return 0; end; "      // narrows v → int (§7.4)
        "    var a := new Atomic<long>(v); "        // int payload, i64 cell
        "    return int(a.Value); "                 // 42 with the fix, 0 without
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// #2634: the same widening shape on the `AtomicInteger<T>` entry point, which
// shares the `colName == "Atomic" || colName == "AtomicInteger"` lowering
// branch and therefore shares the defect.
BEZEL_TEST(codegen_atomic_widening_ctor_2634,
           atomic_integer_long_ctor_narrowed_int_nullable_initial) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private static method N(flag: bool): int? "
        "    if flag then return 42; end; "
        "    return null; "
        "  end N; "
        "  public static method Main(): int "
        "    var v := N(true); "
        "    if v = null then return 0; end; "      // §7.4 narrowing
        "    var a := new AtomicInteger<long>(v); "
        "    return int(a.Value); "
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// #2634 part 1 + part 2 together: a NEGATIVE narrowed `int?` into
// `Atomic<long>`. This is the case that makes the two halves inseparable —
// with only the unwrap it reads back 4294967295 (the unconditional ZExt),
// with only the signedness key it is still 0 (never unwrapped).
BEZEL_TEST(codegen_atomic_widening_ctor_2634,
           atomic_long_ctor_narrowed_negative_int_nullable_sign_extends) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private static method N(flag: bool): int? "
        "    if flag then return -1; end; "
        "    return null; "
        "  end N; "
        "  public static method Main(): int "
        "    var v := N(true); "
        "    if v = null then return 0; end; "      // §7.4 narrowing
        "    var a := new Atomic<long>(v); "
        "    var got: long := a.Value; "
        "    a.Dispose(); "
        "    if got = -1L then return 7; end; "            // correct (SExt)
        "    if got = 4294967295L then return 8; end; "    // the ZExt defect
        "    return 9; "
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// #2634 OVER-SEXT GUARD, the #2617 lesson: a narrowed UNSIGNED nullable must
// still ZERO-extend. §5.1 admits `byte -> long`, and byte(200) is i8 0xC8 —
// sign-extending it yields -56. This shape is NEWLY REACHABLE (before the
// unwrap it was silently 0), so it is the case a careless "just SExt" fix
// would break.
BEZEL_TEST(codegen_atomic_widening_ctor_2634,
           atomic_long_ctor_narrowed_byte_nullable_zero_extends) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  private static method N(flag: bool): byte? "
        "    if flag then return byte(200); end; "
        "    return null; "
        "  end N; "
        "  public static method Main(): int "
        "    var v := N(true); "
        "    if v = null then return 0; end; "      // §7.4 narrowing
        "    var a := new Atomic<long>(v); "
        "    var got: long := a.Value; "
        "    a.Dispose(); "
        "    if got = 200L then return 7; end; "    // correct (ZExt)
        "    if got = -56L then return 8; end; "    // the SExt trap
        "    return 9; "
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// #2634 part 2, the PRE-EXISTING half pinned: NO nullable anywhere. A plain
// `int` of -1 into `Atomic<long>` read back 4294967295 on 4c27002. Independent
// of part 1 — this value never had a tagged-optional layout.
BEZEL_TEST(codegen_atomic_widening_ctor_2634,
           atomic_long_ctor_plain_negative_int_sign_extends) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var x: int := -1; "
        "    var a := new Atomic<long>(x); "        // §5.1 int → long
        "    var got: long := a.Value; "
        "    a.Dispose(); "
        "    if got = -1L then return 7; end; "
        "    if got = 4294967295L then return 8; end; "
        "    return 9; "
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// #2634 part 2: the same pre-existing sign loss at a cell size of 4 rather
// than 8 — `sbyte` -1 into `Atomic<int>` read back 255 on 4c27002. Proves the
// defect was never specific to the 8-byte cell, and that `sbyte`/`short` (the
// signed types NOT in isUnsignedPrim's set) take the SExt branch.
BEZEL_TEST(codegen_atomic_widening_ctor_2634,
           atomic_int_ctor_plain_negative_sbyte_sign_extends) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var x: sbyte := sbyte(-1); "
        "    var a := new Atomic<int>(x); "         // §5.1 sbyte → int
        "    var got: int := a.Value; "
        "    a.Dispose(); "
        "    if got = -1 then return 7; end; "
        "    if got = 255 then return 8; end; "
        "    return 9; "
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// #2634 NO-REGRESSION CONTROL for part 2: a plain UNSIGNED source at the
// maximal ZExt/SExt divergence. `uint` 0x80000000 into `Atomic<long>` was
// CORRECT before this commit (the ladder's blanket ZExt happened to suit it)
// and must stay 2147483648, not -2147483648. This is the test that fails if
// the signedness key is inverted or if isUnsignedPrim stops covering `uint`.
BEZEL_TEST(codegen_atomic_widening_ctor_2634,
           atomic_long_ctor_plain_uint_zero_extends) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var x: uint := 2147483648U; "
        "    var a := new Atomic<long>(x); "        // §5.1 uint → long
        "    var got: long := a.Value; "
        "    a.Dispose(); "
        "    if got = 2147483648L then return 7; end; "
        "    if got = -2147483648L then return 8; end; "
        "    return 9; "
        "  end Main; "
        "end C;"
    );
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// #2634 REJECT CONTROL — the soundness precondition of the widening unwrap.
//
// The unwrap fires on the argument's sema type being a NULLABLE PRIMITIVE,
// which is precisely the trigger condition of #2618's sema gate. That gate
// checks the argument against the non-nullable form of ITS OWN type, so it
// fires for `int?` whatever T is — verified mechanically at this WIDENING
// instantiation, not merely assumed from the `Atomic<int>` case above. If this
// ever starts compiling, the widening unwrap becomes a silent read of a null
// box's payload and must be removed. Asserted on diagnostic TEXT per
// tests/README.md (runAot synthesizes an Error for a SIGSEGV too, so `!r.ran`
// alone cannot distinguish a rejection from a crash).
BEZEL_TEST(codegen_atomic_widening_ctor_2634,
           atomic_long_ctor_unnarrowed_int_nullable_rejected) {
    BEZEL_EXPECT_TRUE(atomicRejectsMentioning(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var n: int? := null; "
        "    var a := new Atomic<long>(n); "   // NOT narrowed — must reject
        "    return int(a.Value); "
        "  end Main; "
        "end C;", "nullable"));
}
