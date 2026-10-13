// fix #2822 — `await` on an IMPORTED async method's `Task<T>` is rejected
// (§30.2 await, §30.3 Task/OwningTask, §2.5 Dependency resolution,
// §2.8.2 The artifact interface is a semantic requirement — v0.240).
//
// MEASURED on the parent commit, real two-module build:
//     app.bzl:5:14: `await` requires a `Task`, `Task<T>`, or `OwningTask<T>`
//     operand (§30.2 / §30.3).
//   -> bezelc rc=1, no binary. The identical program in ONE module compiles and
//      returns the right value.
//
// That makes EVERY imported `async` method unusable, because §30 gives no other
// way to consume a `Task<T>` — so the §2.8.2 clause 2 obligation ("an external
// instantiation behaves identically to a same-module instantiation") fails for
// the whole async surface of any dependency, not for one shape.
//
// THE CAUSE — an AST-SHAPE test standing in for the question actually asked.
// Both the reject guard and the result-typing block gated on
//
//     ot->astNode && ot->astNode->name && ot->astNode->name->parts.size() == 1
//
// before consulting `headName()`. An IMPORTED type has NO ast::Type node by
// construction — ModuleLoader builds its TypeRefs from metadata, there is no AST
// to point at — so an imported `Task<int>` failed the precondition and was
// judged "not a Task". `TypeRef::headName()` already falls back to
// `importedName` for exactly this case, so the head was recoverable all along;
// only the precondition was wrong. This is the #2374 `astNode`-only gate family.
//
// ⚠ WHY THE TWO BLOCKS ARE NOW ONE PREDICATE, and why that is not cosmetic.
// The guard decides whether to REJECT; the block below it decides the await's
// RESULT TYPE. They carried the same condition as two copies. Lifting only the
// first would have made the program compile with the await left UNTYPED — a
// silent wrong value in place of a loud rejection, which is strictly worse than
// the bug being fixed. They share one definition so they cannot drift.
//
// The `parts.size() == 1` half is KEPT where an AST node exists: it rejects a
// QUALIFIED spelling (`Foo.Task`) that must not be taken for the builtin. For an
// imported ref the metadata name is already the bare head, so "importedName is
// non-empty" is the equivalent condition.
//
// ⚠ EVERY CASE HERE ASSERTS A VALUE, not just "it compiles". The issue's own
// repro only printed a string and returned 0, which would pass even with the
// result type wrong — exactly the vacuity this suite has to avoid, since the
// result-typing block is half of what the fix touches.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

constexpr int kDepFailed2822 = -2;

struct TempFile2822 {
    std::string path;
    explicit TempFile2822(const std::string& p) : path(p) {}
    ~TempFile2822() { std::remove(path.c_str()); }
};

void write2822(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build A -> object, then B importing A -> exe, run, return the exit code.
// -1 means the IMPORTING compile or the run failed — which is this defect's
// symptom, so it is the red state, not a harness problem.
int runXmod2822(const std::string& stem, const std::string& aSrcText,
                const std::string& bSrcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + unique_stem("");
    TempFile2822 aSrc((dir / (tag + "_a.bzl")).string());
    TempFile2822 aObj((dir / (tag + "_a.o")).string());
    write2822(aSrc.path, aSrcText);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepFailed2822;
    }
    if (!std::ifstream(aObj.path).good()) return kDepFailed2822;
    TempFile2822 bSrc((dir / (tag + "_b.bzl")).string());
    TempFile2822 bExe((dir / (tag + "_b")).string());
    write2822(bSrc.path, bSrcText);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

int runSingle2822(const std::string& stem, const std::string& srcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + unique_stem("");
    TempFile2822 src((dir / (tag + ".bzl")).string());
    TempFile2822 exe((dir / (tag + "_x")).string());
    write2822(src.path, srcText);
    bezel::Driver driver;
    if (!driver.compileFile(src.path, exe.path, {}).success) return -1;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// True iff compiling `srcText` as a single module FAILS. Used by the
// over-acceptance controls: widening what counts as a Task must not make a
// non-Task operand acceptable.
bool singleModuleRejects2822(const std::string& stem,
                             const std::string& srcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + unique_stem("");
    TempFile2822 src((dir / (tag + ".bzl")).string());
    TempFile2822 exe((dir / (tag + "_x")).string());
    write2822(src.path, srcText);
    bezel::Driver driver;
    return !driver.compileFile(src.path, exe.path, {}).success;
}

const char* kLibTaskT =
    "module LA2822A; "
    "public class Runner "
    "  public static async method Work(i: int): Task<int> "
    "    var built := $\"as-{i}\"; "
    "    return built.Length; "
    "  end Work; "
    "end Runner;";

}  // namespace

// ===========================================================================
// T1 — THE ISSUE'S REPRO, strengthened to assert the VALUE. `await` on an
// imported `Task<int>` was a compile error; it must now compile AND yield 4
// (the length of "as-7").
// ===========================================================================
BEZEL_TEST(xmod_imported_async_await_2822,
           t1_await_imported_task_of_t_yields_the_value_spec_30_3) {
    int rc = runXmod2822(
        "bz2822_t1", kLibTaskT,
        "module Main; import LA2822A; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var a := await Runner.Work(7); "
        "    return a; "
        "  end Run; "
        "  public static method Main(): int "
        "    using var t := C.Run(); return t.Result; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed2822);
    if (rc == kDepFailed2822) return;
    BEZEL_EXPECT_EQ(rc, 4);
}

// ===========================================================================
// T2 — §2.8.2 clause 2 CONTROL: the identical declarations as ONE module. This
// defines the required answer; T1 is a divergence claim against it.
// ===========================================================================
BEZEL_TEST(xmod_imported_async_await_2822,
           t2_single_module_control_spec_2_8_2) {
    int one = runSingle2822(
        "bz2822_t2",
        "module Main; "
        "public class Runner "
        "  public static async method Work(i: int): Task<int> "
        "    var built := $\"as-{i}\"; "
        "    return built.Length; "
        "  end Work; "
        "end Runner; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var a := await Runner.Work(7); return a; "
        "  end Run; "
        "  public static method Main(): int "
        "    using var t := C.Run(); return t.Result; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(one, 4);
}

// ===========================================================================
// T3 — the OTHER TWO §30.3 forms, which the same guard governed: an imported
// plain `Task` (awaits to unit — observed through a side effect, since there is
// no value to assert) and an imported `OwningTask<T>` (awaits to T). 55 + 3 = 58.
// A fix that handled only `Task<T>` passes T1 and fails here.
// ===========================================================================
BEZEL_TEST(xmod_imported_async_await_2822,
           t3_await_imported_plain_task_and_owningtask_spec_30_3) {
    int rc = runXmod2822(
        "bz2822_t3",
        "module LA2822B; "
        "public class R2 "
        "  public static Trace: int; "
        "  public static async method Bump(): Task "
        "    R2.Trace := 55; "
        "  end Bump; "
        "  public static async method Own(i: int): OwningTask<Tok> "
        "    return new Tok(i); "
        "  end Own; "
        "end R2; "
        "public sealed class Tok "
        "  public readonly property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "  public method Len(): int return N; end Len; "
        "end Tok;",
        "module Main; import LA2822B; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    await R2.Bump(); "
        "    using var s := await R2.Own(3); "
        "    return R2.Trace + s.Len(); "
        "  end Run; "
        "  public static method Main(): int "
        "    using var t := C.Run(); return t.Result; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed2822);
    if (rc == kDepFailed2822) return;
    BEZEL_EXPECT_EQ(rc, 58);
}

// ===========================================================================
// T4 — OVER-ACCEPTANCE CONTROL. The fix WIDENS what the guard accepts, so the
// thing to prove is that it did not widen it to everything: `await` on a plain
// integer must still be rejected with the §30.2 diagnostic.
// ===========================================================================
BEZEL_TEST(xmod_imported_async_await_2822,
           t4_await_on_a_non_task_still_rejected_spec_30_2) {
    BEZEL_EXPECT_TRUE(singleModuleRejects2822(
        "bz2822_t4",
        "module Main; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var a := await 5; return a; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;"));
}

// ===========================================================================
// T5 — OVER-ACCEPTANCE CONTROL: `await` on a plain STRING is likewise still
// rejected. A second non-Task shape, because the guard now reaches a name-based
// path and a name-based path is the kind that goes too permissive quietly.
// ===========================================================================
BEZEL_TEST(xmod_imported_async_await_2822,
           t5_await_on_a_string_still_rejected_spec_30_2) {
    BEZEL_EXPECT_TRUE(singleModuleRejects2822(
        "bz2822_t5",
        "module Main; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    var s := \"nope\"; var a := await s; return 1; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;"));
}

// ===========================================================================
// T6 — ACCEPT-CONTROL: a LOCAL (non-imported) async await, which always worked,
// must keep working. This is the guard that the precondition was RELAXED, not
// replaced — a fix that switched to the imported path only would fail here.
// ===========================================================================
BEZEL_TEST(xmod_imported_async_await_2822,
           t6_local_async_await_unaffected_spec_30_3) {
    int one = runSingle2822(
        "bz2822_t6",
        "module Main; "
        "class C "
        "  public static async method Work(): Task<int> return 4; end Work; "
        "  public static async method Run(): Task<int> "
        "    var a := await C.Work(); return a; "
        "  end Run; "
        "  public static method Main(): int "
        "    using var t := C.Run(); return t.Result; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(one, 4);
}

}  // namespace bezel::module
