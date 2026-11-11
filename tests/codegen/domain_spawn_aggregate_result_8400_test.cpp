// Codegen — §35.12 / stdlib §6.8 (#8400): a `Domain.Spawn` entry whose
// result is a sendable RECORD or a value-like CHOICE (`Result<T, E>`) must
// round-trip end-to-end — the aggregate crosses as a COPY of the value
// itself, and the awaiting domain reads it back as the type it is.
//
// Regression for #8400: `Domain.Spawn(entry)` where `entry` returns a
// sendable value-like record, or a value-like choice such as
// `Result<int, LoadError>`, aborted code generation. The result was widened
// with `zext` as if it were an integer:
//
//   ZExt only operates on integer
//     %spawn.ret.zext = zext %record.Pt %1 to i64
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// (and, for the choice, `zext %"choice.Result$spec0" %1 to i64` with the
// same failure). The sibling arm of the same marshalling site — a LAMBDA
// entry (`Domain.Spawn(lambda() => new Pt(1, 2))`) — failed the same way
// with `Invalid bitcast`.
//
// Root cause (fixed):
//   * The spawn adapter marshalled a non-scalar result by widening it to an
//     i64 (`CreateZExt` on the static-method path, `CreateBitCast` on the
//     lambda path). Neither instruction accepts an aggregate, so the IR
//     verifier aborted the module.
//   * The fix marshals an AGGREGATE result as a copy of the value: the
//     adapter heap-copies the value into a block and returns the block's
//     ADDRESS, and `bz_domain_spawn`'s new `result_size` argument tells the
//     runtime to copy the value's BYTES into the join task's result buffer
//     (and free the block). The await site then reads the aggregate back
//     with the by-value `CreateLoad(resultTy, resPtr)` it already used for
//     every other `Task<T>` producer — the same shape an ordinary async
//     method's result takes (`emitAsyncComplete` spills the value and
//     completes the task with `getTypeAllocSize`).
//
// Spec / stdlib:
//   * §35.12 (v0.357) — sendability: "A choice is sendable (§30.14) iff it
//     is value-like and every payload field's type is sendable … a sendable
//     choice crosses as a copy." A record over `int` / `string` is sendable
//     by §30.14's value-type rule.
//   * §30.14 (v0.357) — "A sendable value crosses as a copy."
//   * stdlib §6.8 (v0.357) — `public static method Spawn<R>(entry: () -> R):
//     owned Task<R> where R: sendable, notnull;` ("Value-typed, string or
//     Type result (Pattern A)").
//
// Each test FAILS without the fix (pre-fix: the module does not verify, so
// the program never runs — `runAotOut` reports "<did-not-run>") and PASSES
// with it.
//
// These run through `runAot` (the AOT harness — the load-bearing
// correctness check; each row PRINTS the awaited value and the test asserts
// on the captured stdout) and additionally through an ASan/LSan leg:
//   * the NATIVE leg (`runUnderAsan`, the ASan-runtime-object link the
//     #3653/#4563 family uses) for every row whose program needs no stdlib
//     prelude;
//   * the DRIVER leg (`runAotChecked` — the real driver with
//     `sanitizeAddress = true`) for the builtin-`Result` row: the native
//     leg's mini front end (tests/support/run_under_asan.hpp's
//     `lowerToObjectForAsan`) does not load the stdlib prelude, so it
//     cannot resolve the builtin `Result<T, E>` type at all ("unresolved
//     type name `Result`"), while the real driver — the path the AOT
//     correctness row above takes — resolves it.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
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
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// The awaited value, as the program PRINTS it. `runAotOut` returns the
// program's stdout with ONE trailing line-ending stripped, so the expected
// string is the bare value.
std::string printed(const std::string& program) {
    return runAotOut(program);
}

// ASan probe — link the .bzl with -fsanitize=address so a heap-corruption
// regression in the new aggregate-copy path (double-free, dangling pointer,
// missed free) shows up under the sanitizer.
int runUnderASan(const std::string& source) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsan(source, "bezel_8400_asan_");
}

// The value channel AND the native memory net: the printed value must be
// the awaited one, and the program must be ASan/LSan-clean.
void expectPrinted(const std::string& src, const std::string& expected) {
    BEZEL_EXPECT_EQ(printed(src), expected);
    int san = runUnderASan(src);
    if (san != -1) BEZEL_EXPECT_EQ(san, 0);
}

// The value channel AND the driver memory net — for the rows whose program
// needs the stdlib prelude (the builtin `Result<T, E>`), which the native
// leg's mini front end cannot resolve.
void expectPrintedChecked(const std::string& src,
                          const std::string& expected) {
    BEZEL_EXPECT_EQ(printed(src), expected);
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (bezel::test::checkedToolchainSkipped(r)) return;
    // ran == false means EITHER a compile rejection OR a sanitizer report
    // (ASan use-after-free / LSan leak) — both fail this row.
    BEZEL_EXPECT_TRUE(r.ran);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// #8400 — the issue's own record repro. `Domain.Spawn(Program.Make)` where
// `Make` returns the record `Pt(X: int, Y: int)`; the awaiting domain reads
// `p.X + p.Y` (3). Pre-fix the module did not verify
// (`zext %record.Pt %1 to i64`), so nothing ran at all.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           record_result_round_trips_through_the_domain) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public record Pt(X: int, Y: int); "
        "public class Program "
        "  public static method Make(): Pt => new Pt(1, 2); "
        "  public static async method Main(): Task "
        "    using var t := Domain.Spawn(Program.Make); "
        "    var p := await t; "
        "    Console.WriteLine(p.X + p.Y); "
        "  end Main; "
        "end Program;";
    expectPrinted(src, "3");
}

// ─────────────────────────────────────────────────────────────────────────────
// #8400 — the issue's own choice repro. `Domain.Spawn(Program.Load)` where
// `Load` returns the value-like choice `Result<int, LoadError>`; the awaiting
// domain matches the Error arm and reads `e.Code` (2). Pre-fix the module did
// not verify (`zext %"choice.Result$spec0" %1 to i64`).
//
// The choice's payload record carries a `string` field, so this row also
// pins that the choice's ACTIVE payload crosses intact — the payload's
// string is a literal here (a process-lifetime static), so the row stays
// free of the separate runtime-string leak filed alongside this fix.
//
// The ASan leg here is the DRIVER one (`runAotChecked`): the builtin
// `Result<T, E>` comes from the stdlib prelude, which the native leg's
// mini front end does not load.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           choice_result_round_trips_through_the_domain) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public record LoadError(Code: int, Path: string); "
        "public class Program "
        "  public static method Load(): Result<int, LoadError> "
        "    return Error(new LoadError(2, \"app.toml\")); "
        "  end Load; "
        "  public static async method Main(): Task "
        "    using var t := Domain.Spawn(Program.Load); "
        "    case await t of "
        "      Ok(var n)    => Console.WriteLine(n), "
        "      Error(var e) => Console.WriteLine(e.Code) "
        "    end case; "
        "  end Main; "
        "end Program;";
    expectPrintedChecked(src, "2");
}

// ─────────────────────────────────────────────────────────────────────────────
// #8400 — the int CONTROL. The scalar result path is unchanged by the fix
// (the adapter still packs the int's bits into the pointer-sized word the
// runtime copies), so this row proves the aggregate arm did not regress it.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           int_result_control_still_round_trips) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public class Program "
        "  public static method Make(): int => 42; "
        "  public static async method Main(): Task "
        "    using var t := Domain.Spawn(Program.Make); "
        "    var n := await t; "
        "    Console.WriteLine(n); "
        "  end Main; "
        "end Program;";
    expectPrinted(src, "42");
}

// ─────────────────────────────────────────────────────────────────────────────
// #8400 — the string CONTROL. A `string` result is a runtime-managed
// pointer-sized value (§6.8's "Value-typed, string or Type result
// (Pattern A)"), so it crosses as the pointer it always was.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           string_result_control_still_round_trips) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public class Program "
        "  public static method Make(): string => \"hello\"; "
        "  public static async method Main(): Task "
        "    using var t := Domain.Spawn(Program.Make); "
        "    var s := await t; "
        "    Console.WriteLine(s); "
        "  end Main; "
        "end Program;";
    expectPrinted(src, "hello");
}

// ─────────────────────────────────────────────────────────────────────────────
// #8400 — the USER-DEFINED choice twin of the builtin-`Result` row above:
// the same value-like shape (an int payload and a record payload), spelled
// with a `choice` declaration so the NATIVE ASan leg can compile it. It
// pins that the aggregate arm is about the SHAPE (a value-like choice),
// not about the builtin `Result` spelling.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           user_defined_choice_result_round_trips) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public record LoadError(Code: int, Path: string); "
        "public choice Loaded "
        "  Ok(n: int), "
        "  Error(e: LoadError); "
        "end Loaded; "
        "public class Program "
        "  public static method Load(): Loaded "
        "    return Loaded.Error(new LoadError(2, \"app.toml\")); "
        "  end Load; "
        "  public static async method Main(): Task "
        "    using var t := Domain.Spawn(Program.Load); "
        "    case await t of "
        "      Loaded.Ok(var n)    => Console.WriteLine(n), "
        "      Loaded.Error(var e) => Console.WriteLine(e.Code) "
        "    end case; "
        "  end Main; "
        "end Program;";
    expectPrinted(src, "2");
}

// ─────────────────────────────────────────────────────────────────────────────
// #8400 — the LAMBDA entry spelling. §6.8's `Spawn<R>(entry: () -> R)` is
// spelled with a lambda as often as with a method reference, and the
// sibling arm of the same marshalling site failed the same way
// (`Invalid bitcast` on `%record.Pt`). Pre-fix the module did not verify.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           lambda_entry_record_result_round_trips) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public record Pt(X: int, Y: int); "
        "public class Program "
        "  public static async method Main(): Task "
        "    using var t := Domain.Spawn(lambda() => new Pt(1, 2)); "
        "    var p := await t; "
        "    Console.WriteLine(p.X + p.Y); "
        "  end Main; "
        "end Program;";
    expectPrinted(src, "3");
}

// ─────────────────────────────────────────────────────────────────────────────
// #8400 — the SPAWN-ARGUMENT sibling: an entry that takes a sendable
// record argument AND returns a sendable record result. The argument side
// already heap-copied a struct (#3181 / #3239); this row pins that the
// RESULT side of the same call is the aggregate copy, and that the two do
// not interfere (the argument's copy is freed by the adapter; the result's
// copy crosses to the awaiting domain).
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           record_arg_and_record_result_round_trip_together) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public record Pt(X: int, Y: int); "
        "public class Program "
        "  public static method Shift(v: Pt): Pt => new Pt(v.X + 1, v.Y + 1); "
        "  public static async method Main(): Task "
        "    using var t := Domain.Spawn(Program.Shift, new Pt(1, 2)); "
        "    var p := await t; "
        "    Console.WriteLine(p.X + p.Y); "
        "  end Main; "
        "end Program;";
    expectPrinted(src, "5");
}
