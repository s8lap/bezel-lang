// Codegen — virtual GENERIC overload dispatch resolves the WRONG overload
// (issue #5113, §22.6 / §14.2 / §12, v0.307).
//
// Root cause: `emitVirtualGenericDispatch`'s `mostDerived` lambda
// (`src/codegen/LlvmBackend_Calls2.cpp` ~16108) resolves the runtime callee
// purely by NAME via `findMethodByName(c, ms->name)`, ignoring the overload
// identity of `ms`. When a class declares two `virtual` generic methods
// sharing a name, every dynamic-dispatch call resolves to whichever one
// `findMethodByName` returns first-declared — regardless of which overload
// the call site's arguments actually selected at sema time. This is the
// GENERIC twin of #1085 (which fixed the non-generic virtual overload
// dispatch; that fix re-keyed `buildVtable` by name+arity, but
// `emitVirtualGenericDispatch` does NOT consult the vtable slot for generic
// methods — every override is a per-(class, type-arg) monomorphized body, and
// the dispatch site picks the override itself, by name, here).
//
// Measured repro (no overload ambiguity at the static call site — sema
// resolves `g.Qux(5, 6)` to the 2-arg overload; the dynamic dispatch then
// re-resolves by name and grabs the 1-arg body):
//     class B
//       public constructor() end constructor;
//       public virtual method Qux<T>(a: T): int return 91; end Qux;
//       public virtual method Qux<T>(a: T, b: T): int return 93; end Qux;
//     end B;
//     class C
//       public static method Main(): int
//         var g: B := new B();
//         return g.Qux(5, 6);
//       end Main;
//     end C;
// Pre-fix `g.Qux(5, 6)` returns 91 (the 1-param body, first-declared); the
// 2-param body (93) is never reached.
//
// Fix: `mostDerived` matches the SPECIFIC overload of `ms` reachable from
// class `c` (same name + same generic-param count + same param arity + same
// param types), not the first method with the same name. Other call sites
// of `findMethodByName` (`LlvmBackend_Calls2.cpp:8319, 11423`) are
// name-then-arity-narrowed to single-param contract methods (`CompareTo`
// /`Equals`) or single-overload extension-method devirtualization and are
// left alone.
//
// Spec references:
//   §22.6 / §14.2 — virtual generic method dispatch selects the override
//                    declared on the runtime type, by signature (not name).
//   §12           — dynamic dispatch on virtual methods.
//   v0.307

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"

namespace fs = std::filesystem;

namespace {

int run(const std::string& prog) {
    auto r = bezel::test::runAot(prog);
    if (!r.ran) return -999;
    // 8-bit exit-code truncation (CLAUDE.md "vacuous-oracle traps"): sums
    // above 255 wrap. Read the value from stdout instead.
    try {
        return std::stoi(r.stdoutText);
    } catch (...) {
        return r.exitCode;
    }
}

// Loop amplification (the §17 "one-shot leak" trap): a wrong-overload
// dispatch returns the FIRST-declared body for every call, so summing
// across many iterations collapses into a single distinctive wrong value.
// 64 iterations is a power of two; the correct sum (93 * 64 = 5952) and
// the wrong sum (91 * 64 = 5824) are far apart, with no off-by-one risk
// from the call count itself.
constexpr int kIters = 64;

}  // namespace

// ── T1 — HEADLINE repro: two virtual generic overloads, distinct arity ────
//
// `B` declares two `virtual Qux<T>` overloads. `g.Qux(5, 6)` (the 2-arg form)
// must dispatch to the 2-arg body returning 93 — NOT to the first-declared
// 1-arg body returning 91. Summed over 64 iterations the wrong value is
// 91 * 64 = 5824; the correct value is 93 * 64 = 5952.
//
// Spec §22.6 / §14.2 / §12, v0.307.
BEZEL_TEST(overloaded_virtual_generic_dispatch_5113,
           t1_distinct_arity_two_arg_dispatch_22_6_14_2_12) {
    const char* prog =
        "module Pph; "
        "public class B "
        "  public constructor() end constructor; "
        "  public virtual method Qux<T>(a: T): int return 91; end Qux; "
        "  public virtual method Qux<T>(a: T, b: T): int return 93; end Qux; "
        "end B; "
        "class App "
        "  public static method Main(): int "
        "    var g: B := new B(); "
        "    var sum: int := 0; "
        "    var i: int := 0; "
        "    while i < 64 do "
        "      sum := sum + g.Qux(5, 6); "   // MUST be 93 each time
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(sum); "        // 5952 post-fix; 5824 pre-fix
        "    return 0; "
        "  end Main; "
        "end App;";
    BEZEL_EXPECT_EQ(run(prog), 93 * kIters);
}

// And the 1-arg form must reach the 1-arg body (91), not silently start
// hitting the wrong overload after the fix narrows by signature. Pre-fix
// it happened to reach 91 too (first-declared name match) — this row
// guards that the fix didn't *flip* to the wrong overload.
BEZEL_TEST(overloaded_virtual_generic_dispatch_5113,
           t1_distinct_arity_one_arg_dispatch_22_6_14_2_12) {
    const char* prog =
        "module Pph; "
        "public class B "
        "  public constructor() end constructor; "
        "  public virtual method Qux<T>(a: T): int return 91; end Qux; "
        "  public virtual method Qux<T>(a: T, b: T): int return 93; end Qux; "
        "end B; "
        "class App "
        "  public static method Main(): int "
        "    var g: B := new B(); "
        "    var sum: int := 0; "
        "    var i: int := 0; "
        "    while i < 64 do "
        "      sum := sum + g.Qux(5); "      // MUST be 91 each time
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(sum); "        // 5824 (=91*64)
        "    return 0; "
        "  end Main; "
        "end App;";
    BEZEL_EXPECT_EQ(run(prog), 91 * kIters);
}

// ── T2 — same-arity same-name, distinct PARAM TYPES (#5101 shape) ────────
//
// `EqF<T>(a: T, b: string)` (returns 61) and `EqF<T>(a: T, b: T)` (returns
// 63) have the same name, same arity, same generic-param count — only the
// second param's type differs. A name-only match returns whichever is
// first-declared; the (a: T, b: T) call below is then routed to the
// (a: T, b: string) body, which reads `b` as a string pointer and crashes
// or returns garbage. Looping amplifies the wrong value.
//
// Spec §22.6 / §14.2 / §12, v0.307.
BEZEL_TEST(overloaded_virtual_generic_dispatch_5113,
           t2_same_arity_distinct_param_type_22_6_14_2_12) {
    const char* prog =
        "module Pph; "
        "public class B "
        "  public constructor() end constructor; "
        "  public virtual method EqF<T>(a: T, b: string): int return 61; end EqF; "
        "  public virtual method EqF<T>(a: T, b: T): int return 63; end EqF; "
        "end B; "
        "class App "
        "  public static method Main(): int "
        "    var g: B := new B(); "
        "    var sum: int := 0; "
        "    var i: int := 0; "
        "    while i < 64 do "
        "      sum := sum + g.EqF(5, 5); "   // T,T overload → 63
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(sum); "        // 4032 post-fix
        "    return 0; "
        "  end Main; "
        "end App;";
    BEZEL_EXPECT_EQ(run(prog), 63 * kIters);
}

// ── T3 — positive control: NON-virtual overloaded generic method ──────────
//
// A statically-dispatched (non-virtual) overload set MUST keep working — the
// fix is scoped to the virtual-generic dispatch path. Two overloads of `Foo`
// differ by arity; `p.Foo(7)` reaches the 1-arg body (71) and `p.Foo(7,8)`
// reaches the 2-arg body (73). No regression row.
BEZEL_TEST(overloaded_virtual_generic_dispatch_5113,
           t3_positive_control_nonvirtual_overloaded_generic_22_6) {
    const char* prog =
        "module Pph; "
        "public class P "
        "  public constructor() end constructor; "
        "  public method Foo<T>(a: T): int return 71; end Foo; "
        "  public method Foo<T>(a: T, b: T): int return 73; end Foo; "
        "end P; "
        "class App "
        "  public static method Main(): int "
        "    var p: P := new P(); "
        "    if p.Foo(7) = 71 and p.Foo(7, 8) = 73 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end App;";
    BEZEL_EXPECT_EQ(run(prog), 1);
}

// ── T4 — virtual generic overload dispatch via a BASE-typed receiver ──────
//
// Same shape as T1 but the call goes through `var g: Base := d;` — the
// dynamic-dispatch path is now exercised (Base's vtable holds Qux entries
// from the most-derived class on the static side; `mostDerived` runs over
// the candidate list and must pick the 2-arg overload specifically).
BEZEL_TEST(overloaded_virtual_generic_dispatch_5113,
           t4_base_typed_receiver_two_arg_22_6_14_2_12) {
    const char* prog =
        "module Pph; "
        "public class Base "
        "  public constructor() end constructor; "
        "  public virtual method Qux<T>(a: T): int return 81; end Qux; "
        "  public virtual method Qux<T>(a: T, b: T): int return 83; end Qux; "
        "end Base; "
        "public class Der: Base "
        "  public constructor() end constructor; "
        "  public override method Qux<T>(a: T): int return 91; end Qux; "
        "  public override method Qux<T>(a: T, b: T): int return 93; end Qux; "
        "end Der; "
        "class App "
        "  public static method Main(): int "
        "    var d: Der := new Der(); "
        "    var g: Base := d; "
        "    var sum: int := 0; "
        "    var i: int := 0; "
        "    while i < 64 do "
        "      sum := sum + g.Qux(5, 6); "   // MUST be 93 (Der 2-arg)
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(sum); "        // 5952 post-fix
        "    return 0; "
        "  end Main; "
        "end App;";
    BEZEL_EXPECT_EQ(run(prog), 93 * kIters);
}

// ── T5 — cross-module: an IMPORTED generic virtual overload must still
//          resolve to the right overload after the boundary ────────────────
//
// §14.2 cross-module reach row: `DepB` lives in module `M` (built as
// DepM.o), `App` imports `M` and dispatches `g.Qux(5, 6)` through a
// `DepB`-typed receiver. The import boundary must not lose overload
// identity (the same `mostDerived` path runs on the importer side too —
// `importedGenericLacksConstraintBody5009` and friends populate the same
// override-resolution tables). The fix is in the dispatch path itself,
// not the importer, so this row is also a regression guard for the
// cross-module direction.

namespace {

struct TempFile5113 {
    std::string path;
    explicit TempFile5113(std::string p) : path(std::move(p)) {}
    ~TempFile5113() { std::remove(path.c_str()); }
};

void writeSource5113(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct LibHandle5113 {
    std::shared_ptr<TempFile5113> src;
    std::shared_ptr<TempFile5113> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle5113 buildLib5113(const fs::path& dir,
                           const std::string& libSrc) {
    LibHandle5113 h;
    std::string stem = unique_stem("bezel_5113lib_");
    h.src = std::make_shared<TempFile5113>(
        (dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile5113>(
        (dir / (stem + "_lib.o")).string());
    writeSource5113(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r =
        driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct InProcessApp5113 {
    bool ok = false;
    std::shared_ptr<TempFile5113> src;
    std::shared_ptr<TempFile5113> exe;
    std::string exePath;
};

InProcessApp5113 buildApp5113(const fs::path& dir,
                              const std::string& libObjPath,
                              const std::string& appSrc) {
    InProcessApp5113 r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem("bezel_5113app_");
    r.src = std::make_shared<TempFile5113>(
        (dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile5113>(
        (dir / (stem + "_app")).string());
    writeSource5113(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb =
        driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

struct RunResult5113 { int code; std::string out; };

RunResult5113 runCapture5113(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

static const fs::path& wtDir5113() {
    return wtScratchDir("wt-5113");
}

}  // namespace

BEZEL_TEST(overloaded_virtual_generic_dispatch_5113,
           t5_xmod_imported_overload_dispatch_22_6_14_2_12) {
    auto dir = wtDir5113();
    const std::string lib =
        "module Lib5113; "
        "public class DepB "
        "  public constructor() end constructor; "
        "  public virtual method Qux<T>(a: T): int return 71; end Qux; "
        "  public virtual method Qux<T>(a: T, b: T): int return 73; end Qux; "
        "end DepB; ";
    const std::string app =
        "module App5113; "
        "import Lib5113; "
        "public class C "
        "  public static method Main(): int "
        "    var g: DepB := new DepB(); "
        "    var sum: int := 0; "
        "    var i: int := 0; "
        "    while i < 64 do "
        "      sum := sum + g.Qux(5, 6); "  // MUST be 73 each time
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(sum); "       // 4672 post-fix (8-bit-truncated to 64 if returned)
        "    return 0; "
        "  end Main; "
        "end C; ";
    LibHandle5113 libH = buildLib5113(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp5113 r = buildApp5113(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(r.ok);
    if (!r.ok) return;
    auto [code, out] = runCapture5113(r.exePath);
    int got = -999;
    try { got = std::stoi(out); } catch (...) {}
    BEZEL_EXPECT_EQ(got, 73 * kIters);
}
