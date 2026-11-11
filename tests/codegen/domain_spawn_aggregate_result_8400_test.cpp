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

// ═════════════════════════════════════════════════════════════════════════════
// fix-forward #8400 — the FAULT path. §30.14 (v0.357): a fault in the entry
// "surfaces at the join as `DomainFaultedException`" (stdlib §6.8), and the
// program must stay leak-free on that path too. The aggregate arm's adapter
// heap-copies the entry's result into a block after the entry returns
// WITHOUT testing `bz_exc`, so a faulting entry still hands the runtime a
// block; `bz_domain_spawn`'s fault branch did not free it, leaking sizeof(R)
// per faulting spawn (measured by the rejecting review: 800 B in 100
// allocations for the static-method loop below).
//
// Each row spawns a faulting entry 100 times (a one-shot leak hides in the
// sanitizer's noise), catches every `DomainFaultedException`, PRINTS the
// count, and runs under the driver ASan/LSan leg (`runAotChecked`), whose
// LeakSanitizer report turns `ran` false.
// ═════════════════════════════════════════════════════════════════════════════
namespace {

void expectCleanFaultLoop(const std::string& src) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (bezel::test::checkedToolchainSkipped(r)) return;
    // ran == false: a compile rejection OR a sanitizer report (the leak).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

}  // namespace

// The review's t5_throw / t13_throw_loop: a static-method entry returning the
// record `Pt` that throws, spawned 100 times.
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           faulting_record_entry_frees_its_result_block_100x) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public record Pt(X: int, Y: int); "
        "public class Program "
        "  public static method Make(): Pt "
        "    throw new Exception(\"boom\"); "
        "  end Make; "
        "  public static async method Main(): Task "
        "    var n := 0; "
        "    for var i in 0..<100 do "
        "      using var t := Domain.Spawn(Program.Make); "
        "      try "
        "        var p := await t; "
        "        n := n + p.X; "
        "      catch var e: DomainFaultedException => "
        "        n := n + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(n); "
        "  end Main; "
        "end Program;";
    expectCleanFaultLoop(src);
}

// The choice twin (the review's t12): a static-method entry returning the
// value-like choice `Result<int, LoadError>` that throws, spawned 100 times.
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           faulting_choice_entry_frees_its_result_block_100x) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public record LoadError(Code: int, Path: string); "
        "public class Program "
        "  public static method Load(): Result<int, LoadError> "
        "    throw new Exception(\"boom\"); "
        "  end Load; "
        "  public static async method Main(): Task "
        "    var n := 0; "
        "    for var i in 0..<100 do "
        "      using var t := Domain.Spawn(Program.Load); "
        "      try "
        "        case await t of "
        "          Ok(var k)    => n := n + k, "
        "          Error(var e) => n := n + e.Code "
        "        end case; "
        "      catch var e: DomainFaultedException => "
        "        n := n + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(n); "
        "  end Main; "
        "end Program;";
    expectCleanFaultLoop(src);
}

// The LAMBDA adapter arm (the review's t5b): a lambda entry whose body calls
// a throwing method returning the record `Named`, spawned 100 times.
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           faulting_lambda_record_entry_frees_its_result_block_100x) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public record Named(Tag: string, N: int); "
        "public class Program "
        "  public static method Boom(): Named "
        "    throw new Exception(\"boom\"); "
        "  end Boom; "
        "  public static async method Main(): Task "
        "    var n := 0; "
        "    for var i in 0..<100 do "
        "      using var t := Domain.Spawn(lambda() => Program.Boom()); "
        "      try "
        "        var p := await t; "
        "        n := n + p.N; "
        "      catch var e: DomainFaultedException => "
        "        n := n + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(n); "
        "  end Main; "
        "end Program;";
    expectCleanFaultLoop(src);
}

// ═════════════════════════════════════════════════════════════════════════════
// fix-forward #8400 — a NULLABLE result is not a Spawn instantiation.
// stdlib §6.8 (v0.357) binds every result-bearing row's R under
// `where R: sendable, notnull` (the plain `Task<R>` rows) or
// `where R: sendable, class` (the `OwningTask<R>` rows), and §14.2 (v0.357)
// excludes "a nullable type, value or reference" from both. So an entry
// returning `Pt?` (the review's t9b_nullable_value) is a compile error.
// Before this the call was admitted, and on #8400's tip it compiled to a
// program that printed garbage for `3` / `null` and read 12 bytes out of an
// 8-byte join buffer (heap-buffer-overflow).
// ═════════════════════════════════════════════════════════════════════════════
namespace {

void expectNullableResultRejected(const std::string& src) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "Domain.Spawn entry result type is nullable"));
}

}  // namespace

// The review's t9b_nullable_value: a static-method entry `Make(k): Pt?`.
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           nullable_record_result_entry_is_rejected) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public record Pt(X: int, Y: int); "
        "public class Program "
        "  public static method Make(k: int): Pt? "
        "    if k > 0 then return new Pt(1, 2); end if; "
        "    return null; "
        "  end Make; "
        "  public static async method Main(): Task "
        "    using var t := Domain.Spawn(Program.Make, 1); "
        "    var p := await t; "
        "    if p <> null then Console.WriteLine(p.X + p.Y); "
        "    else Console.WriteLine(\"null\"); end if; "
        "    using var u := Domain.Spawn(Program.Make, 0); "
        "    var q := await u; "
        "    if q <> null then Console.WriteLine(q.X + q.Y); "
        "    else Console.WriteLine(\"null\"); end if; "
        "  end Main; "
        "end Program;";
    expectNullableResultRejected(src);
}

// The nullable SCALAR twin: §14.2's exclusion is not about the record shape
// (`int?` is excluded from `notnull` exactly as `Pt?` is).
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           nullable_int_result_entry_is_rejected) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public class Program "
        "  public static method Make(): int? => null; "
        "  public static async method Main(): Task "
        "    using var t := Domain.Spawn(Program.Make); "
        "    var p := await t; "
        "    if p <> null then Console.WriteLine(p); "
        "    else Console.WriteLine(\"null\"); end if; "
        "  end Main; "
        "end Program;";
    expectNullableResultRejected(src);
}

// The nullable REFERENCE twin through the LAMBDA spelling: `string?` is
// excluded from `notnull` ("value or reference", §14.2).
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           nullable_string_lambda_result_entry_is_rejected) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public class Program "
        "  public static method Find(): string? => null; "
        "  public static async method Main(): Task "
        "    using var t := Domain.Spawn(lambda() => Program.Find()); "
        "    var s := await t; "
        "    if s <> null then Console.WriteLine(s); "
        "    else Console.WriteLine(\"null\"); end if; "
        "  end Main; "
        "end Program;";
    expectNullableResultRejected(src);
}

// The OWNED-row twin: `() -> owned Report?` binds R = `Report?` in
// `Spawn<R>(entry: () -> owned R) where R: sendable, class`, and §14.2's
// `class` accepts "Non-nullable reference types only". (Its non-nullable
// twin, `() -> owned Report`, is admitted and runs.)
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           nullable_owned_class_result_entry_is_rejected) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public sealed class Report end Report; "
        "public class Program "
        "  public static method Make(): owned Report? => null; "
        "  public static async method Main(): Task "
        "    using var t := Domain.Spawn(Program.Make); "
        "    Console.WriteLine(\"ran\"); "
        "  end Main; "
        "end Program;";
    expectNullableResultRejected(src);
}

// The admitting twin of the row above: the non-nullable owned class result
// is a legal `OwningTask<Report>` instantiation, so the nullability check
// must not fire on it.
BEZEL_TEST(domain_spawn_aggregate_result_8400,
           non_nullable_owned_class_result_entry_still_runs) {
    std::string src =
        "module P; "
        "import System.Threading; "
        "public sealed class Report end Report; "
        "public class Program "
        "  public static method Make(): owned Report => new Report(); "
        "  public static async method Main(): Task "
        "    using var t := Domain.Spawn(Program.Make); "
        "    using var r := await t; "
        "    Console.WriteLine(\"ran\"); "
        "  end Main; "
        "end Program;";
    expectPrintedChecked(src, "ran");
}
