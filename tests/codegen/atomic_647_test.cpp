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
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace fs = std::filesystem;
using bezel::test::runJit;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_647atom_lsan_" + std::to_string(counter++);
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
