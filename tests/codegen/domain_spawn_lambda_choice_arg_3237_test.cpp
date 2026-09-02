// Codegen — §30.14 / stdlib §6.8 (#3237): a non-capturing lambda spawn
// entry with a struct-typed (choice) value argument must lower to valid
// LLVM IR (no IR-verifier abort) and must round-trip the value.
//
// Regression for #3237: `Domain.Spawn(lambda(c: CInt) => ..., CInt.One(99))`
// — a non-capturing lambda entry whose user parameter is a choice (§35.12
// sendable value-like type) — aborted compilation with two stacked IR
// defects that fire together:
//
//   1. Sender-side: the lambda adapter's multi-arg packing loop fell
//      through to `CreateBitCast(val, i64()) → CreateIntToPtr` for any
//      non-integer / non-pointer value, so `%choice.CInt { i32, {i8},
//      {i32} }` (12 bytes) was bitcast to `i64` — `Invalid bitcast`,
//      verifier abort. The static-method adapter's `spawnValueToVoidPtr`
//      already had the heap-copy fix (#3181 / sharpen #3181) for
//      structs, but the lambda adapter's packing loop had its own
//      inline coercion and never adopted it.
//
//   2. Receiver-side state leak: a SYNC lambda body nested inside an
//      `async method Main` had the enclosing machine's `smSelf_` /
//      `smStateStruct_` / `asyncResumeFn_` / … fields un-cleared while
//      its own block-body emitted, so `emitReturn` routed the case
//      arm's `return 0` through `emitAsyncComplete` — which reads
//      `smSelf_` as the enclosing async frame — and emitted e.g.
//      `bz_task_complete(... %choice.CInt %1)` in the lambda's own
//      `() -> int` function where `%1` is the user's `CInt` param.
//      That produced three "Referring to an argument in another
//      function!" errors pointing at
//      `getelementptr %async.Main, ptr %1, 0, 4` — the verifier saw
//      `%1` (the lambda's CInt arg) typed as `%async.Main*`, the
//      enclosing machine's state struct. The async-lambda branch
//      already save/restored these fields around `emitAsyncLambda`;
//      the sync branch silently left them.
//
// Root cause (fixed):
//   * The lambda adapter's multi-arg packing now uses the same
//     `spawnValueToVoidPtr(val, expectedTy)` the static-method adapter
//     uses, so a struct (or float / double) extra arg rides on the
//     heap-copy path. `castSlotToParamTy` mirrors `castToParamTy`'s
//     struct/float/double arm — `Load(paramTy, BitCast(voidPtr,
//     paramTy*))` — instead of falling through to `PtrToInt`. The
//     receiver therefore produces the param's actual type, not i64,
//     and the indirect call to the lambda matches the lambda fn type.
//   * `lowerLambda` now also save/restores the same async-state fields
//     across the SYNC branch's body emit. The async-only comment was
//     wrong: a sync lambda's body emit goes through `emitReturn`, which
//     short-circuits to `emitAsyncComplete` whenever `smSelf_` is
//     non-null, so the same enclosing-machine hazard applied.
//
// These run through `runAot` (the AOT harness) — the load-bearing
// correctness check — and additionally through ASan-linked native
// binaries. The exit code is the int returned by `Main(): Task<int>`.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"
#include "test_harness.hpp"

#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "codegen/LlvmBackend.hpp"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

int runUnderASan(const std::string& source) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsan(source, "bezel_3237_asan_");
}

void expectCleanAndEq(const std::string& src, int expectedExit) {
    BEZEL_EXPECT_EQ(run(src), expectedExit);
    int san = runUnderASan(src);
    if (san != -1) BEZEL_EXPECT_EQ(san, 0);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// #3237 — the headline repro from the issue body. Single non-capturing
// lambda spawn entry, single choice-typed arg carrying a payload. Pre-
// fix the verifier aborted on `i64 bitcast (%choice.CInt …)` plus three
// "Referring to an argument in another function!" errors pointing at
// `%async.Main` GEPs on the lambda's CInt param (`%1`). Post-fix the
// choice round-trips through the spawn boundary, the One(99) payload
// reaches the case arm, and the entry returns 99.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_lambda_choice_arg_3237,
           non_capturing_lambda_entry_with_choice_arg_round_trips) {
    std::string src =
        "module SpawnLambdaChoice3237; "
        "import System.Threading; "
        "public choice CInt "
        "  Zero, "
        "  One(n: int); "
        "end CInt; "
        "class P "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(lambda(c: CInt) => do "
        "      case c of "
        "        Zero => do return 0; end, "
        "        One(var n) => do return n; end "
        "      end case; "
        "    end, CInt.One(42)); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end P;";
    expectCleanAndEq(src, 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// #3237 — tag-only (payload-free) sibling. Proves the heap-copy path
// round-trips the choice's tag bytes too, not just payloads.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_lambda_choice_arg_3237,
           non_capturing_lambda_entry_with_tag_only_choice_arg) {
    std::string src =
        "module SpawnLambdaChoiceZero; "
        "import System.Threading; "
        "public choice CInt "
        "  Zero, "
        "  One(n: int); "
        "end CInt; "
        "class P "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(lambda(c: CInt) => do "
        "      case c of "
        "        Zero => do return 0; end, "
        "        One(var n) => do return n; end "
        "      end case; "
        "    end, CInt.Zero); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end P;";
    expectCleanAndEq(src, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// #3237 — the static-method control from the same codegen site. Confirms
// the static-method adapter (§30.14 / stdlib §6.8
// `Spawn<A, R>(entry: (A) -> R, arg: A)` with A = choice CInt, R = int)
// still works for this input — the lambda fix is a strict superset of
// the static-method shape and must not regress the sibling path.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_lambda_choice_arg_3237,
           static_method_entry_with_choice_arg_still_round_trips) {
    std::string src =
        "module SpawnStaticChoice3237; "
        "import System.Threading; "
        "public choice CInt "
        "  Zero, "
        "  One(n: int); "
        "end CInt; "
        "class P "
        "  public static method Entry(v: CInt): int "
        "    case v of "
        "      Zero => do return 0; end, "
        "      One(var n) => do return n; end "
        "    end case; "
        "  end Entry; "
        "end P; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(P.Entry, CInt.One(42)); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end C;";
    expectCleanAndEq(src, 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// #3237 — same shape but with a record (not choice) value arg. Records
// are also struct-typed sendable values per §30.14 / §35.12 and ride on
// the same heap-copy path the lambda fix adopts; this pins the path is
// uniform across value-like struct kinds.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_lambda_choice_arg_3237,
           non_capturing_lambda_entry_with_record_arg_round_trips) {
    std::string src =
        "module SpawnLambdaRec; "
        "import System.Threading; "
        "public record Pair "
        "  public x: int; "
        "  public y: int; "
        "end Pair; "
        "class P "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(lambda(p: Pair) => do "
        "      return p.x + p.y * 7; "
        "    end, new Pair(x := 1, y := 10)); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end P;";
    expectCleanAndEq(src, 71);  // 1 + 10*7
}