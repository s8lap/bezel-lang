// Codegen — issue #2701: reading a block-bodied INDEXER (`property [i: int]: T
// get => do … end;`) from an ASYNC method emitted malformed IR.
//
// Spec (revision v0.240):
//   §23.4 "Indexers" — an indexer's accessors take the same `=> expression` /
//     `=> do … end` forms as a block-form property (§23.3). A `do … end`
//     accessor is a user statement body lowered into its OWN function; its
//     `return` terminates the ACCESSOR.
//   §30.5 "Result lifetime and Task disposal" — an `async` method's `return`
//     completes the outer task. That completion pathway belongs to the async
//     method; it must not fire inside a non-async helper function that the
//     async body merely calls.
//
// THE BUG. #2170 fixed `ensurePropertyBlockGetter` (and
// `ensurePropertyAccessorFn`) to save/null/restore `smKind_`/`smStateStruct_`,
// because a lazily-emitted getter must not inherit the caller's state-machine
// emission mode. `ensureIndexerBlockGetter` (LlvmBackend_Assign.cpp) is the
// INDEXER spelling of the identical function — it lowers the same kind of user
// statement body via `lowerScopedBlock(acc->stmts)` — and was never given that
// treatment. With `smKind_ == Await` and `smStateStruct_` still pointing at the
// CALLER's frame, the indexer body's `return _n` took the async-return path and
// emitted `bz_task_complete` + spill-free + frame-free INTO the indexer getter,
// so the getter's own `ret i32` was never emitted. Observed pre-fix:
//     Referring to an argument in another function!
//       %6 = getelementptr inbounds nuw %async.RunAsync, ptr %1, i32 0, i32 3
//     Function return type does not match operand type of return inst!
//       ret void
//      i32
//     bezelc: codegen error: IR verification failed
//
// THE FIX. `ensureIndexerBlockGetter` adopts `Emitter::NestedBodyScope`, the
// guard introduced by the #2702 commit, which covers the #2170 pair together
// with the residual per-body set.
//
// ⚠ WHY THE TWO ISSUES HAD TO LAND TOGETHER, AND WHY THE GUARD IS THE FIX.
// #2170's reset value for `smKind_` is `SuspendKind::Yield` — the enum has no
// neutral member — and `Yield` is exactly what OPENS lowerTry's second gate
// `smStateGEP_ && (smKind_ == Yield || smKind_ == AsyncYield)`
// (LlvmBackend_Stmts.cpp). Mirroring #2170's two-field save here WITHOUT also
// nulling `smStateGEP_` would have moved the failure rather than removed it:
// case (2) below (an indexer body containing a `try`) is the shape that proves
// it, and it needs both halves.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

// Plain (non-ASan) AOT run — checked alongside the sanitizer oracle because
// `--full` has masked live defects the plain gate catches (#2633 / #2629).
int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent — case skipped
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// (1) THE ISSUE'S REPRO. A block-bodied `int` indexer read from an `async`
// method. Pre-fix the module fails IR verification, so the program never runs
// and the harness yields the -999 sentinel. Post-fix the indexer returns 7 and
// Main returns 7 - 7 + 3 = 3. §23.4 + §30.5.
static const char* kAsyncIndexerRead =
    "module M; "
    "class Owner "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property [i: int]: int "
    "    get => do "
    "      return _n; "
    "    end; "
    "  end; "
    "end Owner; "
    "class P "
    "  public static async method RunAsync(): Task<int> "
    "    using var o := new Owner(); "
    "    return o[0]; "
    "  end RunAsync; "
    "  public static async method Main(): Task<int> "
    "    using var t := P.RunAsync(); "
    "    var r := await t; "
    "    return r - 7 + 3; "          // 3
    "  end Main; "
    "end P;";

BEZEL_TEST(codegen_indexer_block_getter_async_2701, block_indexer_read_from_async) {
    BEZEL_EXPECT_EQ(runPlain(kAsyncIndexerRead), 3);
}

BEZEL_TEST(codegen_indexer_block_getter_async_2701,
           block_indexer_read_from_async_checked) {
    int rc = runChecked(kAsyncIndexerRead);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 3);
}

// (2) THE COMBINED SHAPE — the one that proves the state-machine pair alone is
// not enough. The indexer body wraps its `return` in a `try … finally`, so it
// reaches lowerTry's `smStateGEP_` paths in addition to the async-return path.
// A fix that mirrored only #2170's two fields would still emit a store into the
// CALLER's function here. Value: the indexer returns 9 + 2 = 11, Main returns
// 11 - 8 = 3. §23.4 + §30.5 + §31.2.
static const char* kAsyncIndexerWithTry =
    "module M; "
    "class Bag "
    "  private _n: int; "
    "  public constructor() _n := 9; end constructor; "
    "  public property [i: int]: int "
    "    get => do "
    "      try "
    "        return _n + i; "
    "      finally "
    "        var z := 1; "
    "      end; "
    "    end; "
    "  end; "
    "end Bag; "
    "class P "
    "  public static async method RunAsync(): Task<int> "
    "    using var b := new Bag(); "
    "    return b[2]; "               // 11
    "  end RunAsync; "
    "  public static async method Main(): Task<int> "
    "    using var t := P.RunAsync(); "
    "    var r := await t; "
    "    return r - 8; "              // 3
    "  end Main; "
    "end P;";

BEZEL_TEST(codegen_indexer_block_getter_async_2701, try_in_block_indexer_from_async) {
    BEZEL_EXPECT_EQ(runPlain(kAsyncIndexerWithTry), 3);
}

BEZEL_TEST(codegen_indexer_block_getter_async_2701,
           try_in_block_indexer_from_async_checked) {
    int rc = runChecked(kAsyncIndexerWithTry);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 3);
}

// (3) The indexer's `finally` must still RUN — the fix must neutralise the
// CALLER's suspend frame without disabling the indexer body's own try
// machinery. The finally's side effect is folded into the returned value, so a
// skipped finally is a wrong exit code rather than a crash. §31.2:
// "`finally` runs whether the protected block completed normally, was exited by
// an exception (caught or uncaught), or was exited by `return` …".
// _n starts 9; the finally sets _k := 5; Main reads K after the read, so
// 9 + 5 = 14, minus 11 = 3.
static const char* kFinallyRunsInIndexer =
    "module M; "
    "class Bag "
    "  private _n: int; "
    "  private _k: int; "
    "  public constructor() _n := 9; _k := 0; end constructor; "
    "  public property [i: int]: int "
    "    get => do "
    "      try "
    "        return _n; "
    "      finally "
    "        _k := 5; "
    "      end; "
    "    end; "
    "  end; "
    "  public property K: int "
    "    get => _k; "
    "  end K; "
    "end Bag; "
    "class P "
    "  public static async method RunAsync(): Task<int> "
    "    using var b := new Bag(); "
    "    var a := b[0]; "
    "    return a + b.K; "            // 9 + 5 = 14 only if the finally ran
    "  end RunAsync; "
    "  public static async method Main(): Task<int> "
    "    using var t := P.RunAsync(); "
    "    var r := await t; "
    "    return r - 11; "             // 3
    "  end Main; "
    "end P;";

BEZEL_TEST(codegen_indexer_block_getter_async_2701, finally_still_runs_in_block_indexer) {
    BEZEL_EXPECT_EQ(runPlain(kFinallyRunsInIndexer), 3);
}

// (4) CONTROL — the same block-bodied indexer read from an ORDINARY (non-async)
// method must keep working. This is the case that catches an over-correction in
// the guard. It passes with and without the fix, by design. §23.4.
static const char* kSyncIndexerControl =
    "module M; "
    "class Owner "
    "  private _n: int; "
    "  public constructor() _n := 7; end constructor; "
    "  public property [i: int]: int "
    "    get => do "
    "      return _n + i; "
    "    end; "
    "  end; "
    "end Owner; "
    "class P "
    "  public static method Main(): int "
    "    using var o := new Owner(); "
    "    return o[1] - 5; "           // 8 - 5 = 3
    "  end Main; "
    "end P;";

BEZEL_TEST(codegen_indexer_block_getter_async_2701, sync_block_indexer_control) {
    BEZEL_EXPECT_EQ(runPlain(kSyncIndexerControl), 3);
}
