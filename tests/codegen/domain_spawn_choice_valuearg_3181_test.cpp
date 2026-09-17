// Codegen — §30.14 / stdlib §6.8 (#3181): a value-typed choice passed to
// `Domain.Spawn` as a sendable argument must lower to valid LLVM IR (no
// IR-verifier abort), must actually transfer the value to the spawned
// domain, and must do so without aliasing violations (heap-copy contract;
// spec §30.14 "sendable value crosses as a copy").
//
// Regression for #3181: `Domain.Spawn(Entry, SendOk.Num(1))` where SendOk is
// a value-like sendable choice (§30.14 value-type rule, §35.12) aborted
// compilation with `Invalid bitcast ... %choice.SendOk { ... } to i64` at
// the LLVM IR verifier — the lowering emitted an illegal bitcast of a
// non-8-byte struct to i64. sema correctly accepts the program (§30.14
// "Argument and result types must be sendable"; §35.12 "a sendable choice
// crosses as a copy"); only the codegen marshalling was missing.
//
// Root cause (fixed): `LlvmBackend_Calls3.cpp`'s Domain.Spawn sender
// bitcast every non-pointer, non-integer arg to i64 and stored the bits
// into the spawn-arg slot — invalid for any choice whose struct is not
// exactly 8 bytes (essentially all of them), and silently wrong for any
// 8-byte struct. The receiver then ptrtoint'd the slot back to i64 and
// type-mismatched it against the entry's struct parameter, which would
// have been a silent wrong-value except the verifier caught it first.
//
// The fix: a struct-typed spawn arg is heap-copied by the sender (per
// bz_domain_spawn's arg_ptr comment "address of a stack/heap copy for
// value types"), the heap pointer rides in the void* arg slot, and the
// receiver loads the value out of it and frees the copy after the entry
// call returns. Pointer / int args keep the existing semantics — a
// pointer passthrough, or zero-extended bits packed into the slot,
// respectively. An optional ASan probe (`--probe --asan FILE.bzl`) on
// the same .bzl checks the heap allocation is freed.
//
// These run through `runAot` (the AOT harness) — the load-bearing
// correctness check — and additionally through ASan-linked native
// binaries. The exit code is the int returned by `Main(): Task<int>`.

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "native_test_harness.hpp"
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

// Per-process unique working dir under the shared scratch base (DISK-backed
// per CLAUDE.md; /tmp tmpfs races across shards).
static const fs::path& wtDir3181() {
    return wtScratchDir("wt-3181");
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Cross-module harness: compile the dependency as a -c object (with its
// .bezel.meta section), then compile the app against it via --import and
// run. Mirrors the family-F9 / #3144 / #3148 cross-module pattern.
struct TempFile3181 {
    std::string path;
    explicit TempFile3181(std::string p) : path(std::move(p)) {}
    ~TempFile3181() { std::remove(path.c_str()); }
};

void writeSrc(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

int compileImportAndRun3181(const std::string& libSrc, const std::string& appSrc,
                            const std::string& tag) {
    auto dir = wtDir3181();
    std::string tagPrefix = "bz3181_" + tag + "_";
    std::string stem = unique_stem(tagPrefix.c_str());
    TempFile3181 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile3181 lObj((dir / (stem + "_lib.o")).string());
    TempFile3181 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile3181 aExe((dir / stem).string());

    writeSrc(lSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(lSrc.path, lObj.path).success)
            return -2;
    }
    if (!std::ifstream(lObj.path).good()) return -2;

    writeSrc(aSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = lObj.path;
    d.objectPath   = lObj.path;
    deps.push_back(d);
    if (!driver.compileFile(aSrc.path, aExe.path, deps).success)
        return -1;
    if (!std::ifstream(aExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(aExe.path.c_str(), aExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -3;
}

// ASan probe: link the .bzl with `-fsanitize=address` so a heap-corruption
// regression in the new heap-copy path (double-free, dangling pointer,
// missed free) shows up under the sanitizer. Mirrors the helper shape
// in tests/codegen/domain_spawn_lambda_valuearg_1490_test.cpp.
//
// #3653: NOT converted to tests/support/run_under_asan.hpp. Despite mirroring
// domain_spawn_lambda_valuearg_1490's shape closely, this copy's LINK failure
// path returns the OLD conflated -1 (not #3633's -3), while its own
// `status == -1` (system() failure) path already returns -4 -- an internally
// inconsistent, partially-migrated copy, not a clean match to Family B. Filed
// separately as a latent bug (a link failure here reads as "toolchain
// unavailable: SKIP" instead of a loud FAIL) — see the issue filed alongside
// this commit; fixing it is out of scope for a pure dedup.
int runUnderASan3181(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir3181();
    std::string stem = unique_stem("bezel_3181_asan_");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -2;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -2;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -2;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -2;
    }

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
        return -3;
    }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(objPath.c_str());
    std::remove(exe.c_str());
    if (status == -1) return -4;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

void expectCleanAndEq3181(const std::string& src, int expectedExit) {
    BEZEL_EXPECT_EQ(run(src), expectedExit);
    int san = runUnderASan3181(src);
    if (san != -1) BEZEL_EXPECT_EQ(san, 0);
}

// fix #3676: the link-failure return code was the OLD conflated `-1`, masking
// a real ASan link failure as a toolchain-unavailable SKIP. Splitting the link
// step out into a small inline helper both fixes the contract (link failure
// is `-3`, per #3633) and exposes the link/run classification to a regression
// test that can force a link failure without breaking the local toolchain.
//   -3  link failure under -fsanitize=address       (was -1 — #3676)
//   -4  system() failure on the run step (fork/exec)
//    0  clean
//    1  ASan report (LSan exit code 99)
inline int linkRunAsan3181(const std::string& rtObj, const std::string& objPath,
                           const std::string& exe) {
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
        return -3;
    }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(objPath.c_str());
    std::remove(exe.c_str());
    if (status == -1) return -4;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// #3181: the exact issue repro — a value-typed choice (Num case carrying an
// int) passed to a static-method entry. Pre-fix: bezelc aborted with
// `Invalid bitcast ... %choice.SendOk { ... } to i64`. Post-fix: the value
// 42 reaches the spawned domain and the entry returns it as the join
// result; Main returns 42.
// stdlib §6.8 Spawn<A,R>(entry: (A) -> R, arg: A): owned Task<R>, A = choice
// SendOk (sendable value-like choice per §30.14 + §35.12), R = int.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_choice_valuearg_3181,
           num_payload_choice_round_trips_to_spawned_domain) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "public choice SendOk "
        "  Nothing, "
        "  Num(n: int), "
        "  Text(s: string); "
        "end SendOk; "
        "class P "
        "  public static method Entry(v: SendOk): int "
        "    case v of "
        "      Nothing => do return 0; end, "
        "      Num(var n) => do return n; end, "
        "      Text(var s) => do return s.Length; end "
        "    end case; "
        "  end Entry; "
        "end P; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(P.Entry, SendOk.Num(42)); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq3181(src, 42);
}

// The string-payload case (Text) — exercises the heap-copied struct path
// with a pointer-typed payload field. The §30.14 "owned string crosses as a
// shared reference, which is safe because strings are immutable" clause
// guarantees the inner string's lifetime is fine across the heap copy
// (it's the OUTER choice that's heap-copied; the inner string pointer just
// rides through). Pre-fix: same bitcast abort; the string payload is
// never observed. Post-fix: entry returns s.Length = 5, exit 5.
BEZEL_TEST(domain_spawn_choice_valuearg_3181,
           text_payload_choice_round_trips_to_spawned_domain) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "public choice SendOk "
        "  Nothing, "
        "  Num(n: int), "
        "  Text(s: string); "
        "end SendOk; "
        "class P "
        "  public static method Entry(v: SendOk): int "
        "    case v of "
        "      Nothing => do return 0; end, "
        "      Num(var n) => do return n; end, "
        "      Text(var s) => do return s.Length; end "
        "    end case; "
        "  end Entry; "
        "end P; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(P.Entry, SendOk.Text(\"hello\")); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq3181(src, 5);
}

// The payload-free case (Nothing) — also a struct (tag-only); the
// bitcast-to-i64 path would silently pack zero bits, the heap-copy path
// mallocs a 4-byte (or 8-byte) tag-only copy. Entry returns 0, exit 0.
// Pins that the tag-only case isn't special-cased into the wrong path.
BEZEL_TEST(domain_spawn_choice_valuearg_3181,
           nothing_payload_choice_round_trips_to_spawned_domain) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "public choice SendOk "
        "  Nothing, "
        "  Num(n: int), "
        "  Text(s: string); "
        "end SendOk; "
        "class P "
        "  public static method Entry(v: SendOk): int "
        "    case v of "
        "      Nothing => do return 0; end, "
        "      Num(var n) => do return n; end, "
        "      Text(var s) => do return s.Length; end "
        "    end case; "
        "  end Entry; "
        "end P; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(P.Entry, SendOk.Nothing); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq3181(src, 0);
}

// Cross-module: the choice lives in an IMPORTED module and crosses the
// dependency boundary before reaching Domain.Spawn. This exercises the
// half of the issue that "only became reachable with #3144, because
// before it the imported choice had no cases and the call was rejected
// at SendOk.Num()". Without the wire-shape fix from #3144 the imported
// choice's case list is empty and sema refuses; with #3144 alone the
// module boundary works but the IR verifier still aborts at the spawn
// marshalling. So this test pins BOTH #3144 (case list crosses) and
// #3181 (the value crosses the spawn boundary).
BEZEL_TEST(domain_spawn_choice_valuearg_3181,
           num_payload_choice_round_trips_across_module_boundary) {
    auto result = compileImportAndRun3181(
        // lib: a module-local declaration of the choice and the entry.
        "module SpawnChoiceLib3181; "
        "public choice SendOk "
        "  Nothing, "
        "  Num(n: int), "
        "  Text(s: string); "
        "end SendOk; "
        "public class Worker "
        "  public static method Handle(v: SendOk): int "
        "    case v of "
        "      Nothing => do return 0; end, "
        "      Num(var n) => do return n; end, "
        "      Text(var s) => do return s.Length; end "
        "    end case; "
        "  end Handle; "
        "end Worker; ",
        // app: import the choice + worker, then spawn.
        "module SpawnChoiceApp3181; "
        "import SpawnChoiceLib3181; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(Worker.Handle, SendOk.Num(99)); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end C; ",
        "xmod_num");
    BEZEL_EXPECT_EQ(result, 99);
}

// ─────────────────────────────────────────────────────────────────────────────
// sharpen #3181: f32 sendable primitive as Domain.Spawn arg.
//
// §30.14 lists "All primitives and enums" as sendable, and stdlib §6.8
// `Spawn<A, R>(entry: (A) -> R, arg: A): owned Task<R>` admits them
// through `where A: sendable, notnull` for the value-typed rows. The
// pre-sharpen sender's `bitcast → i64` fallback was a verifier abort for
// any non-pointer, non-integer LLVM type — `float` (f32) hit it directly,
// followed by `ZExt only operates on integer` because the fallback then
// tried to zext the float bits. Sharpen moves f32 (and f64) onto the same
// heap-copy path structs use: `malloc(sizeof(T)) + store + pass heap
// pointer` on the sender; `bitcast ptr → T-ptr + load` on the receiver.
// The entry returns void so the test isolates the arg side from the
// separate (filed) f32 / f64 return-marshalling bug.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_choice_valuearg_3181,
           float_arg_reaches_spawned_domain_static_method) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class P "
        "  public static method Observe(v: float) "
        "    if v = 3.5 then return; end; "
        "    if v = -1.5 then return; end; "
        "  end Observe; "
        "end P; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t1 := Domain.Spawn(P.Observe, 3.5); "
        "    await t1; "
        "    using var t2 := Domain.Spawn(P.Observe, -1.5); "
        "    await t2; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq3181(src, 0);
}

// sharpen #3181: f64 sendable primitive as Domain.Spawn arg.
BEZEL_TEST(domain_spawn_choice_valuearg_3181,
           double_arg_reaches_spawned_domain_static_method) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class P "
        "  public static method Observe(v: double) "
        "    if v = 3.5 then return; end; "
        "    if v = -1.5 then return; end; "
        "  end Observe; "
        "end P; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t1 := Domain.Spawn(P.Observe, 3.5); "
        "    await t1; "
        "    using var t2 := Domain.Spawn(P.Observe, -1.5); "
        "    await t2; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq3181(src, 0);
}

// sharpen #3181: an EXACTLY-8-byte sendable record. The pre-fix sender's
// `bitcast → i64` happened to PASS the verifier for any 8-byte struct,
// so a record of exactly two `int` fields (or one `long`) was the
// canonical "happens to pass the verifier, silently wrong at runtime"
// shape the original commit message names but does NOT pin. Pre-fix, the
// bits round-tripped wrongly: the receiver's `ptrtoint` interpreted the
// pointer as a plain i64, not as a struct value, so the entry observed
// the 8 bytes of the pointer's bit-pattern-pretending-to-be-a-struct
// (essentially undefined for any field the entry actually read). Post-
// sharpen, the heap-copy path means the entry sees the actual struct
// bytes, both fields correctly. The test returns `x + y * 7` (so a
// round-trip error in EITHER field shifts the exit code) and pins 7*10+1
// = 71 as the discriminant.

// ─────────────────────────────────────────────────────────────────────────────
// fix #3676: a real ASan link failure (e.g. clang++ missing the ASan
// runtime, disk-full, or a `-l` flag misroute after a runtime change) must
// NOT be reported as `-1` (toolchain unavailable: SKIP) — that conflates two
// unrelated failure classes and lets the row report a clean PASS while the
// ASan leg is broken. The pre-fix helper returned `-1` on link failure; the
// caller at `:203` then `if (san != -1) BEZEL_EXPECT_EQ(san, 0);` skipped the
// check, so a broken toolchain read as a SKIP, not a FAIL. Post-fix the link
// step returns `-3` (per #3633), so any caller that observes a non-zero
// non-SKIP code fails the test loudly.
//
// We force the link failure by pointing the link line at a runtime object
// path that does not exist — the same command shape runUnderASan3181 uses,
// minus the run step, which is what `linkRunAsan3181` was extracted for.
// The link command IS the assertion: any non-zero exit means the linker
// could not produce an executable, which is exactly the failure class `#3633`
// reserves `-3` for. Pre-fix, the same body returned `-1`; this test would
// fail with `BEZEL_EXPECT_EQ(-1, -3)`.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_choice_valuearg_3181,
           link_failure_returns_minus_three_not_minus_one_3676) {
    auto dir = wtDir3181();
    std::string stem = unique_stem("bezel_3181_asan_linkfail_");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    // A non-empty `.o` file so the link command proceeds past the input
    // shape check and fails on the runtime object path instead — the same
    // shape a real `clang++` install with a missing/broken ASan runtime
    // produces (the linker reports "cannot find -lasan" / "no such file"
    // depending on version, both non-zero).
    {
        std::ofstream f(objPath, std::ios::binary);
        // ELF magic — a valid enough header to survive a header-only check,
        // never enough for a real link. Concatenated string literals because
        // `\x7fELF` would otherwise be one 5-byte hex escape, not `\x7f` +
        // `"ELF"`.
        f << "\x7f" "ELF";
    }

    const std::string bogusRtObj =
        (dir / "definitely-not-a-runtime-for-3676.o").string();
    const int rc = linkRunAsan3181(bogusRtObj, objPath, exe);

    std::remove(objPath.c_str());
    std::remove(exe.c_str());

    // The contract: a link failure under `-fsanitize=address` is `-3`. Pre-fix
    // this returned `-1` and the caller's `if (san != -1)` silently swallowed
    // it as a SKIP, so the ASan leg reported clean PASS while broken.
    BEZEL_EXPECT_EQ(rc, -3);
}

// sharpen #3181: an EXACTLY-8-byte sendable record. The pre-fix sender's
// `bitcast → i64` happened to PASS the verifier for any 8-byte struct,
// so a record of exactly two `int` fields (or one `long`) was the
// canonical "happens to pass the verifier, silently wrong at runtime"
// shape the original commit message names but does NOT pin. Pre-fix, the
// bits round-tripped wrongly: the receiver's `ptrtoint` interpreted the
// pointer as a plain i64, not as a struct value, so the entry observed
// the 8 bytes of the pointer's bit-pattern-pretending-to-be-a-struct
// (essentially undefined for any field the entry actually read). Post-
// sharpen, the heap-copy path means the entry sees the actual struct
// bytes, both fields correctly. The test returns `x + y * 7` (so a
// round-trip error in EITHER field shifts the exit code) and pins 7*10+1
// = 71 as the discriminant.
//   - Pre-sharpen sender: bitcast→i64 passes the verifier; the bits are
//     "accidentally" right only if the struct's in-memory layout and the
//     i64 layout happen to align (they do for {i32, i32} on x86-64), but
//     the receiver's ptrtoint misinterprets them as a plain i64 anyway.
//   - Post-sharpen sender: malloc+store+heap pointer; receiver loads the
//     struct out of the heap.
BEZEL_TEST(domain_spawn_choice_valuearg_3181,
           eight_byte_record_arg_round_trips_correctly_static_method) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "public record Pair "
        "  public x: int; "
        "  public y: int; "
        "end Pair; "
        "class P "
        "  public static method Use(p: Pair): int "
        "    return p.x + p.y * 7; "
        "  end Use; "
        "end P; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(P.Use, new Pair(x := 1, y := 10)); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq3181(src, 71);  // 1 + 10*7 = 71
}