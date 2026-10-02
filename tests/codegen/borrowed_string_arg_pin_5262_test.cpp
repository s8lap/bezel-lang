// Codegen / refcounting — issue #5262, the follow-up to #5244.
//
// #5244 fixed the caller-side pin for a borrowed by-value `string` argument
// across a USER-METHOD call, but scoped out four call shapes. #5262 measured
// all four on the staging tip r23-c28 d68dab26 (`scripts/agent_check.sh
// --probe --asan`) and three of them still dangle:
//
//   shape 1  CONSTRUCTOR    `new Box(h, h._v, "q"+i)`   SEGV in bz$M.Box$ctor
//   shape 2  IMPORTED callee c.Twice(c.Val, ...)         SEGV in bz$Lib.C$Twice
//   shape 3  `string?` FORMAL Twice(x: string?, y: string?) SEGV in
//                    bz$M.C$Twice
//   shape 4  GENERIC T=string                            LEAK, not a dangle
//
// The mechanism is identical in all three dangling shapes and is #5244's: a
// by-value `string` / `string?` parameter owns no +1 of its own
// (retainAssignedStringParams only mints one for a parameter the body ASSIGNS
// to), so `h._v` — passed as an argument, so a borrow of the field's sole
// reference — is freed by the callee's second `lowerStringFieldStore`
// release-old, and the later read of the parameter is a use-after-free.
//
// WHY EACH SHAPE FELL THROUGH #5244's PREDICATE (all three are now closed):
//   shape 1 — a constructor is not a method call: its pin site is `lowerNew`,
//             not `lowerCallDispatch`.
//   shape 2 — the pin was gated on `dynamic_cast<const ast::MethodDecl*>(
//             ms->decl)`, and an IMPORTED method has no ast::MethodDecl
//             (§2.5: the importer has no AST for a dependency's types). The
//             pin now reads the callee's parameter SYMBOLS, which an imported
//             method does carry.
//   shape 3 — the pin required `isStringTypeRefNonNull`, which excludes the
//             nullable form. `bz_string_retain`/`bz_string_release` are both
//             no-ops on null (bezel_runtime_refcounting.cpp:117, :125), so a
//             `string?` formal pins exactly as safely as a `string` one.
//
// SHAPE 4 IS NOT HERE, AND ITS ABSENCE IS THE POINT. At `T = string` the
// generic-body store is #4965's RETAIN-ONLY store (retain-new, never
// release-old), so the class leaks rather than dangles: MEASURED
// `SUMMARY: AddressSanitizer: 1754 byte(s) leaked in 98 allocation(s)`, one
// direct + one indirect pair per iteration, every allocation attributed to
// `main`, with all 50 lines printing correctly first. Flipping that store to
// release-old is #4980's lane and needs this class closed first — which is
// what this file does. A row pinning shape 4 as a leak would freeze #4980's
// starting point, so it is filed rather than pinned here.
//
// Spec: §4.6 "`string`" and §28.9 "Runtime-managed types" — the
// reference-counting implementation is unspecified, so the contract is only
// that a parameter naming a `string` value keeps reading that value for the
// whole call — spec revision v0.313.
//
// THE FIX. `emitBorrowedStringArgPins` (LlvmBackend_impl.hpp) is the one
// caller-side pin, shared by the method-call and constructor call sites so
// the shapes cannot drift apart. Each row builds strings at RUNTIME (a
// literal is an rc sentinel that hides the bug) and is loop-amplified.
// runAotChecked = generated-code ASan + LeakSanitizer.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAotChecked
#include "support/build_paths.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// ── the shared driver ─────────────────────────────────────────────────────
// Every iteration prints the value the parameter named, so the assertion is
// on stdout (never on an exit code — a UAF reads freed memory and can exit 0).
// `prevVal(i)` is the field's value when iteration i starts: "a1" before the
// first store, then the previous iteration's "q" string.
std::string prevVal(int i) {
    return i == 0 ? std::string("a1") : "q" + std::to_string(i - 1);
}

std::string wantPrinted(int iters) {
    std::string w;
    for (int i = 0; i < iters; ++i) w += prevVal(i) + "\n";
    w += "q" + std::to_string(iters - 1) + "\n";
    return w;
}

std::string run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// ═══════════════════════════════════════════════════════════════════════════
// SHAPE 1 — CONSTRUCTOR. `Box`'s constructor stores over `Holder._v` (the
// field `v` aliases) twice and then reads `v`.
// ═══════════════════════════════════════════════════════════════════════════
std::string ctorHolder() {
    return "module M5262a; "
           "public class Holder "
           "  public _v: string; "
           "  public constructor(v: string) _v := v; end constructor; "
           "end Holder; ";
}

std::string ctorSrc(const char* ctorBody, int iters) {
    return ctorHolder() +
           "public class Box "
           "  public constructor(h: Holder, v: string, y: string) " +
           std::string(ctorBody) +
           "  end constructor; "
           "end Box; "
           "public class Program "
           "  public static method Main(): int "
           "    using var h := new Holder(\"a1\"); "
           "    for var i in 0.." "<" + std::to_string(iters) +
           " do "
           "      using var b := new Box(h, h._v, \"q\" + i.ToString()); "
           "    end; "
           "    Console.WriteLine(h._v); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

const char* kCtorTwice = "h._v := v; h._v := y; Console.WriteLine(v);";

// ═══════════════════════════════════════════════════════════════════════════
// SHAPE 2 — IMPORTED callee. `C.Twice` is compiled into Lib5262's artifact;
// the calling module is what pins the borrowed argument.
// ═══════════════════════════════════════════════════════════════════════════
const std::filesystem::path& wtDir5262() {
    return bezel_test::wtScratchDir("wt-5262-codegen");
}

struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

const std::string kImportedLib =
    "module Lib5262; "
    "public class C "
    "  private _v: string; "
    "  public constructor(v: string) _v := v; end constructor; "
    "  public method Twice(x: string, y: string) "
    "    _v := x; "
    "    _v := y; "
    "    Console.WriteLine(x); "
    "  end Twice; "
    "  public property Val: string => _v; "
    "end C; ";

std::string importedApp(int iters) {
    return "module App5262; "
           "import Lib5262; "
           "public class Program "
           "  public static method Main(): int "
           "    using var c := new C(\"a1\"); "
           "    for var i in 0.." "<" + std::to_string(iters) +
           " do "
           "      c.Twice(c.Val, \"q\" + i.ToString()); "
           "    end; "
           "    Console.WriteLine(c.Val); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// The dependency object's TempFile MUST outlive the dependent compile —
// `Driver::compileFile` reopens the `.o` to read its `.bezel.meta` section.
struct ImportedRun {
    std::shared_ptr<TempFile> libSrc, libObj, appSrc, appExe;
    std::string out;
    int code = -1;
    bool built = false;
    bool sawSanitizer = false;
};

ImportedRun runImported(const std::string& appSrc) {
    ImportedRun r;
    auto dir = wtDir5262();
    const std::string stem = bezel_test::unique_stem("bezel_5262");
    r.libSrc = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    r.libObj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    r.appSrc = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.appExe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.libSrc->path, kImportedLib);
    writeSource(r.appSrc->path, appSrc);
    {
        bezel::Driver driver;
        bezel::CompileResult lr =
            driver.compileObject(r.libSrc->path, r.libObj->path);
        if (!lr.success) return r;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = r.libObj->path;
        d.objectPath = r.libObj->path;
        deps.push_back(d);
        bezel::CompileResult ar = bezel::test::compileFileForTest(
            driver, r.appSrc->path, r.appExe->path, deps);
        if (!ar.success) return r;
    }
    r.built = true;
    // Run under LeakSanitizer explicitly: the pin mints a +1 per call, so an
    // unbalanced retain leaks one string per call and an extra release drops
    // the field's own reference. `exitcode=99` is outside every value a
    // correct row returns, so a leak can never be read as a pass. Under the
    // `--full` sweep `compileFileForTest` additionally instruments the
    // generated code and `runProgramForTest` fires its own redzone oracle;
    // this leg is deliberately self-contained so the LSan claim holds in a
    // plain lane run too.
    const std::string cmd =
        "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:exitcode=99 " +
        r.appExe->path + " 2>" + r.appExe->path + ".err";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return r;
    char buf[512];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        r.out.append(buf, n);
    int status = pclose(p);
    r.code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    {
        std::ifstream err(r.appExe->path + ".err");
        std::string text((std::istreambuf_iterator<char>(err)),
                         std::istreambuf_iterator<char>());
        r.sawSanitizer =
            text.find("AddressSanitizer:") != std::string::npos ||
            text.find("LeakSanitizer:") != std::string::npos;
    }
    std::remove((r.appExe->path + ".err").c_str());
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════
// SHAPE 3 — `string?` FORMALS. The callee narrows each parameter before use.
// ═══════════════════════════════════════════════════════════════════════════
std::string nullableSrc(int iters) {
    return "module M5262c; "
           "public class C "
           "  private _v: string; "
           "  public constructor(v: string) _v := v; end constructor; "
           "  public method Twice(x: string?, y: string?) "
           "    if x <> null then _v := x; end if; "
           "    if y <> null then _v := y; end if; "
           "    if x <> null then Console.WriteLine(x); end if; "
           "  end Twice; "
           "  public property Val: string => _v; "
           "end C; "
           "public class Program "
           "  public static method Main(): int "
           "    using var c := new C(\"a1\"); "
           "    for var i in 0.." "<" + std::to_string(iters) +
           " do "
           "      c.Twice(c.Val, \"q\" + i.ToString()); "
           "    end; "
           "    Console.WriteLine(c.Val); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// SHAPE 1 — CONSTRUCTOR CALL
// ═══════════════════════════════════════════════════════════════════════════

// (1a) The issue's shape 1: `new Box(h, h._v, "q"+i)` against a constructor
// that stores over the aliased field twice and then reads the parameter.
// FAIL-WITHOUT (MEASURED on r23-c28 d68dab26, `agent_check.sh --probe
// --asan`): `AddressSanitizer: SEGV on unknown address 0x10`, READ in
// `bz_string_to_utf8z` (bezel_runtime_strings.cpp:1250), frame #1
// `bz$M.Box$ctor`.
BEZEL_TEST(borrowed_string_arg_pin_5262,
           constructor_double_store_ctor_param_survives_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(ctorSrc(kCtorTwice, 50)), wantPrinted(50));
}

// (1b) Control from #5244's own control list: drop the second store. The
// parameter is then never the release-old victim, so this was already clean
// pre-fix — it pins that the pin did not CHANGE this shape.
BEZEL_TEST(borrowed_string_arg_pin_5262,
           control_ctor_single_store_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(
        run(ctorSrc("h._v := v; Console.WriteLine(v);", 50)),
        [] { std::string w; for (int i = 0; i < 51; ++i) w += "a1\n";
             return w; }());
}

// (1c) 200 iterations under LeakSanitizer: the constructor pin must balance
// exactly — a retain without its release leaks one string per construction.
BEZEL_TEST(borrowed_string_arg_pin_5262,
           ctor_pin_200_iters_balanced_no_leak_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(ctorSrc(kCtorTwice, 200)), wantPrinted(200));
}

// ═══════════════════════════════════════════════════════════════════════════
// SHAPE 2 — IMPORTED CALLEE
// ═══════════════════════════════════════════════════════════════════════════

// (2a) `C.Twice` lives in Lib5262's artifact; the app module passes
// `c.Val`, the receiver field's own value.
// FAIL-WITHOUT (MEASURED on r23-c28 d68dab26): SEGV in `bz_string_to_utf8z`,
// frame #1 `bz$Lib5262.C$Twice`.
BEZEL_TEST(borrowed_string_arg_pin_5262,
           imported_callee_double_store_param_survives_spec_4_6_28_9) {
    ImportedRun r = runImported(importedApp(50));
    BEZEL_EXPECT_TRUE(r.built);
    if (!r.built) return;
    BEZEL_EXPECT_FALSE(r.sawSanitizer);
    BEZEL_EXPECT_EQ(r.code, 0);
    BEZEL_EXPECT_EQ(r.out, wantPrinted(50));
}

// (2b) The in-module twin of (2a): the SAME class and the SAME body compiled
// into one module. Already covered by #5244's own rows, so this one is the
// in-module CONTROL for (2a) — it must read identically, which is what makes
// (2a)'s pre-fix failure attributable to the import boundary rather than to
// the program.
BEZEL_TEST(borrowed_string_arg_pin_5262,
           control_in_module_twin_of_imported_row_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(
        run("module M5262b; "
            "public class C "
            "  private _v: string; "
            "  public constructor(v: string) _v := v; end constructor; "
            "  public method Twice(x: string, y: string) "
            "    _v := x; _v := y; Console.WriteLine(x); "
            "  end Twice; "
            "  public property Val: string => _v; "
            "end C; "
            "public class Program "
            "  public static method Main(): int "
            "    using var c := new C(\"a1\"); "
            "    for var i in 0..<50 do "
            "      c.Twice(c.Val, \"q\" + i.ToString()); "
            "    end; "
            "    Console.WriteLine(c.Val); "
            "    return 0; "
            "  end Main; "
            "end Program;"),
        wantPrinted(50));
}

// (2c) 200 iterations across the import boundary under LeakSanitizer.
BEZEL_TEST(borrowed_string_arg_pin_5262,
           imported_pin_200_iters_balanced_no_leak_spec_4_6_28_9) {
    ImportedRun r = runImported(importedApp(200));
    BEZEL_EXPECT_TRUE(r.built);
    if (!r.built) return;
    BEZEL_EXPECT_FALSE(r.sawSanitizer);
    BEZEL_EXPECT_EQ(r.code, 0);
    BEZEL_EXPECT_EQ(r.out, wantPrinted(200));
}

// ═══════════════════════════════════════════════════════════════════════════
// SHAPE 3 — `string?` FORMALS
// ═══════════════════════════════════════════════════════════════════════════

// (3a) `Twice(x: string?, y: string?)` — the nullable formal the pin's
// `isStringTypeRefNonNull` predicate excluded.
// FAIL-WITHOUT (MEASURED on r23-c28 d68dab26): SEGV in `bz_string_to_utf8z`,
// frame #1 `bz$M.C$Twice`.
BEZEL_TEST(borrowed_string_arg_pin_5262,
           nullable_formal_double_store_param_survives_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(nullableSrc(50)), wantPrinted(50));
}

// (3b) Control: a `null` argument. `bz_string_retain`/`bz_string_release` are
// no-ops on null, so pinning a null `string?` argument must retain nothing
// and release nothing — if the pin mishandled null this row would
// over-release, and the next field store would trip #4547's over-release
// report.
BEZEL_TEST(borrowed_string_arg_pin_5262,
           control_null_nullable_formal_pin_is_a_noop_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(
        run("module M5262d; "
            "public class C "
            "  private _v: string; "
            "  public constructor(v: string) _v := v; end constructor; "
            "  public method Take(x: string?, y: string?) "
            "    if x <> null then _v := x; end if; "
            "    if y <> null then _v := y; end if; "
            "  end Take; "
            "  public property Val: string => _v; "
            "end C; "
            "public class Program "
            "  public static method Main(): int "
            "    using var c := new C(\"a1\"); "
            "    for var i in 0..<50 do "
            "      c.Take(c.Val, null); "
            "    end; "
            "    Console.WriteLine(c.Val); "
            "    return 0; "
            "  end Main; "
            "end Program;"),
        "a1\n");
}

// (3c) 200 iterations under LeakSanitizer: the `string?` pin must balance
// exactly, exactly as the `string` one does.
BEZEL_TEST(borrowed_string_arg_pin_5262,
           nullable_pin_200_iters_balanced_no_leak_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(nullableSrc(200)), wantPrinted(200));
}

// ═══════════════════════════════════════════════════════════════════════════
// SHAPE 5 — the INLINE constructor (fix-forward #5262-ff, row 1 of 3)
//
// #5262's pin sat ONLY in lowerNew's `!needsInlineForLayout` arm. But
// `needsInlineForLayout = !genericSubst_.empty()`, so a GENERIC class with
// an ACTIVE substitution routes its constructor body to `inlineCtorBody` —
// in the CALLER's frame, with no call edge. The hazard is identical (the
// inlined body still stores over the aliased field twice, and the borrowed
// parameter owns no +1), so the missing call edge bought nothing.
// ═══════════════════════════════════════════════════════════════════════════

// `new Box<string>(...)` binds a real substitution, so the ctor body is
// inlined into `main`. Same double-store-then-read body as shape 1.
std::string inlineCtorSrc(int iters) {
    return "module M5262e; "
           "public class Holder "
           "  public _v: string; "
           "  public constructor(v: string) _v := v; end constructor; "
           "end Holder; "
           "public class Box<T> "
           "  public constructor(h: Holder, v: string, y: string) "
           "    h._v := v; h._v := y; Console.WriteLine(v); "
           "  end constructor; "
           "end Box; "
           "public class Program "
           "  public static method Main(): int "
           "    using var h := new Holder(\"a1\"); "
           "    for var i in 0.." "<" + std::to_string(iters) +
           " do "
           "      using var b := new Box<string>(h, h._v, \"q\" + i.ToString()); "
           "    end; "
           "    Console.WriteLine(h._v); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// (5a) FAIL-WITHOUT (MEASURED on the rebased r23-c28 9c640754, `agent_check.sh
// --probe --asan`): `AddressSanitizer: SEGV on unknown address 0x10`, READ in
// `bz_string_to_utf8z` (bezel_runtime_strings.cpp:1250), frame #1 `main`,
// printing only `a1` before dying. The first iteration of 200 already dies —
// a UAF is not a slow burn.
BEZEL_TEST(borrowed_string_arg_pin_5262,
           inline_ctor_double_store_param_survives_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(inlineCtorSrc(50)), wantPrinted(50));
}

// (5b) 200 iterations under LeakSanitizer: the inline arm's pin must balance
// exactly, exactly as the call arm's. A retain without its release here would
// leak one string per construction; an over-release would drop the field's own
// reference (the #4547 shape).
BEZEL_TEST(borrowed_string_arg_pin_5262,
           inline_ctor_pin_200_iters_balanced_no_leak_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(inlineCtorSrc(200)), wantPrinted(200));
}

// ═══════════════════════════════════════════════════════════════════════════
// SHAPE 6 — the ASYNC callee (fix-forward #5262-ff, row 2 of 3)
//
// #5262's pin is a CALLER-side retain released on the call's normal-return
// edge. An async callee RETURNS at its first suspension (§30.1: the method
// "suspends and returns its `Task` to the caller"), so that edge arrives long
// before the body finishes — and the body then reads the parameter from a
// different frame, with the borrow already freed by its own second store.
// ═══════════════════════════════════════════════════════════════════════════

// The await sits BETWEEN the two stores and the read, so the read of the
// borrowed parameter happens AFTER a real suspension — which is the whole
// point of the shape. `async Main` awaits each task in turn, so the
// suspensions are sequential and stdout is deterministic (this shape does NOT
// use TaskScope/Spawn, which would make the output order nondeterministic and
// the exact-stdout assertion a flake).
std::string asyncCalleeSrc(int iters) {
    return "module M5262f; "
           "public class C "
           "  private _v: string; "
           "  public constructor(v: string) _v := v; end constructor; "
           "  public async method TwiceAsync(x: string, y: string): Task "
           "    _v := x; "
           "    _v := y; "
           "    await Task.Delay(0); "
           "    Console.WriteLine(x); "
           "  end TwiceAsync; "
           "  public property Val: string => _v; "
           "end C; "
           "public class Program "
           "  public static async method Main(): Task "
           "    using var c := new C(\"a1\"); "
           "    for var i in 0.." "<" + std::to_string(iters) +
           " do "
           "      await c.TwiceAsync(c.Val, \"q\" + i.ToString()); "
           "    end; "
           "    Console.WriteLine(c.Val); "
           "  end Main; "
           "end Program;";
}

// 50 iterations, not 200: an async call per iteration is expensive under the
// AOT harness (the v1 review measured the same reduction and recorded that
// the failure is a FIRST-iteration SEGV, so the count does not weaken the
// evidence). The amplification that matters is inside the single run.
constexpr int kAsyncIters = 50;

// (6a) FAIL-WITHOUT (MEASURED on the rebased r23-c28 9c640754,
// `agent_check.sh --probe --asan`): `AddressSanitizer: SEGV`, READ in
// `bz_string_to_utf8z`, frame #1 `bz$async$M.C$TwiceAsync$Resume`, thread T3
// via `domain_drain` — i.e. the read happens on the RESUME, in a different
// frame from the call, after the caller-side pin was already released.
BEZEL_TEST(borrowed_string_arg_pin_5262,
           async_callee_double_store_param_survives_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(asyncCalleeSrc(kAsyncIters)),
                    wantPrinted(kAsyncIters));
}

// (6b) The same 50 iterations under LeakSanitizer: the frame-entry retain
// must balance at teardown, on BOTH terminal edges. A retain without its
// release leaks one string per task; a double release drops the field's own
// reference (the #4547 shape).
BEZEL_TEST(borrowed_string_arg_pin_5262,
           async_callee_pin_balanced_no_leak_spec_4_6_28_9) {
    BEZEL_EXPECT_EQ(run(asyncCalleeSrc(kAsyncIters)),
                    wantPrinted(kAsyncIters));
}


