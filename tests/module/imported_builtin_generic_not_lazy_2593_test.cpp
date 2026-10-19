// Cross-module regression test for issue #2593's `importedName` guard.
//
// §2.5 / §29.3 (issue #495): ModuleLoader reconstructs a TypeRef for every
// imported generic whose `lookupNamed` fails — which is every BUILTIN
// (`Span<T>`, `Array<T>`, `Dictionary<K, V>`, `IGrouping<TKey, T>`, …), since
// none of them is an exported user type in the dependency. The reconstructed
// TypeRef is `Named` with `declaration == nullptr` AND `astNode == nullptr`,
// carrying the type name ONLY in `importedName` (`Loader::convert`,
// src/module/ModuleLoader.cpp:384-385 — the `declaration == nullptr &&
// !mt.namedName.empty()` guard and its assignment).
//
// That is byte-for-byte the same shape as the LAZY LINQ sentinel that #2593's
// `isLazyEnumerable` predicate keys on. Without an explicit
// `importedName.empty()` term, #2593's §7 argument gate would therefore treat
// EVERY imported builtin generic receiver as a lazy `IEnumerable<T>` and check
// its argument against a non-nullable `genericArgs[0]`.
//
// The shipping false reject that produces: stdlib §4.14 (v0.237, lines
// 2463/2464) declares class-`T` `Span<T>` / `ReadOnlySpan<T>` with NULLABLE
// search parameters — `IndexOf(value: T?)` and `Contains(value: T?)` — so a
// `T?` argument is LEGAL. #2593's `spanBufferSkipClassT` exemption keys on
// `colName == "Span" || "ReadOnlySpan"`, and `colName` is EMPTY for the
// imported form (it reads `headName()` only when `astNode` is non-null), so
// the exemption cannot fire and the legal call is rejected — with a
// diagnostic that also misnames the receiver `IEnumerable<T>.Contains`.
//
// Both sibling copies of this predicate already carry the same guard for the
// same reason — `Emitter::isLazyEnumerableType` in LlvmBackend_Calls3.cpp:35
// (whose comment names §2.5/§29.3 and #495 explicitly) and
// `isLazyEnumerableExprType` in DefiniteAssignment.cpp:1259 ("Must match
// isLazyEnumerableType() in codegen"). This test pins the third.
//
// It is a POSITIVE control, and it asserts the absence of the §7 DIAGNOSTIC
// rather than overall compile success — deliberately. The imported class-`T`
// `Span<R>` path has a SEPARATE, PRE-EXISTING codegen defect ("IR
// verification failed"), filed as #2650, which fails the compile regardless.
// Proved pre-existing by rebuilding with #2593's `isLinqEnumerable` forced
// false: byte-identical failure. Asserting compile success would therefore
// pin someone else's bug and go red for the wrong reason; asserting that no
// §7 nullable diagnostic is emitted pins exactly what #2593 owns.
//
// It cannot be written as a single-module test, because a same-module
// `Span<R>` carries an astNode and never takes the imported shape at all.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) + non-atomic counter pattern was
// race-prone: parallel shards overwrote each other's _lib.bzl / _lib.o /
// _main.bzl mid-compileObject and garbled the in-flight child read. The atomic
// unique_stem + /var/tmp/bezel-scratch parent fixes that class of flake
// (#3002-class COVERAGE SHORT).
const std::filesystem::path& wtDir2593() {
    return wtScratchDir("wt-2593");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::string readAll(const std::string& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// Compile lib -> object, then compile main importing lib, capturing the
// importer's DIAGNOSTIC stream. Returns the captured text (the library's own
// compile is asserted to succeed via the returned marker on failure).
std::string diagnosticsOfCrossModuleImport(const std::string& libSrc,
                                           const std::string& mainSrc) {
    auto dir = wtDir2593();
    std::string stem = unique_stem("bezel_imported_lazy_2593");
    TempFile lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile lObj((dir / (stem + "_lib.o")).string());
    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mObj((dir / (stem + "_main.o")).string());
    TempFile errF((dir / (stem + "_err.txt")).string());

    writeSource(lSrc.path, libSrc);
    writeSource(mSrc.path, mainSrc);

    bezel::Driver driver;
    bezel::CompileResult rl = driver.compileObject(lSrc.path, lObj.path);
    if (!rl.success) return "LIBRARY-COMPILE-FAILED";

    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath   = lObj.path;
    std::vector<bezel::Dependency> deps{dep};

    // Capture stderr around the importer's compile — diagnostics go there.
    std::fflush(stderr);
    int saved = dup(fileno(stderr));
    int sink = open(errF.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(sink, fileno(stderr));
    close(sink);

    driver.compileObject(mSrc.path, mObj.path, deps);

    std::fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);

    return readAll(errF.path);
}

// The §7 "nullable -> non-nullable" assignability diagnostic fragment, and the
// receiver name #2593's gate prints. Either appearing on this program is the
// false reject.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

// A library exporting a class-`T` `Span<R>` producer. `R` IS an exported user
// type (so it resolves), but `Span` is a builtin — `lookupNamed("Span")`
// fails in the importer and the TypeRef comes back astNode-less with
// `importedName == "Span"`.
//
// The library exposes the span via an instance method on a class with an
// `OwningBuffer<R>` field (the §9.7 v0.301 / #4428 receiver-sourced ADMITTED
// arm — `new Span<R>(this.buf, …)` is the §4.14 `FrameBuffer.ScanLine`
// shape). The earlier `static method Get(buf: …): Span<R> => new
// Span<R>(buf, 0, 1);` was a parameter-rooted borrow return and is rejected
// by #4428 (§9.7 v0.301: "a caller has nothing but the receiver to
// re-establish and a parameter-sourced record return would hand back a
// borrow no call site could bound"); the test's #2593 intent is the §7
// nullability gate on the IMPORTER, not the LIBRARY's borrow return shape,
// so the library is reshaped here to keep the §7 path exercised while
// staying out of #4428's reject.
constexpr const char* kLib =
    "module A; "
    "public class R "
    "  public V: int; "
    "  public constructor(v: int) this.V := v; end constructor; "
    "end R; "
    "public class Lib "
    "  public owned buf: OwningBuffer<R>; "
    "  public constructor(owned b: OwningBuffer<R>) this.buf := b; end constructor; "
    "  public method Get(): Span<R> => new Span<R>(this.buf, 0, 1); "
    "end Lib;";

}  // namespace

// #2593 / §2.5 + §29.3 + stdlib §4.14 (v0.237): an IMPORTED class-`T`
// `Span<R>` receiver accepts a nullable `R?` argument to `Contains`, because
// §4.14 declares `Contains(value: T?)` for class `T`. Without the
// `importedName.empty()` term in #2593's `isLazyEnumerable` predicate the
// receiver is mistaken for a lazy `IEnumerable<R>` and this legal program is
// REJECTED with "the argument to `IEnumerable<T>.Contains` is nullable but
// the target type is non-nullable (§7)".
BEZEL_TEST(module_imported_builtin_generic_2593,
           imported_class_t_span_contains_accepts_nullable_argument) {
    std::string diags = diagnosticsOfCrossModuleImport(
        kLib,
        "module B; "
        "import A; "
        "public class Main "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<A.R>(1); "
        "    using var lib := new A.Lib(buf); "
        "    var sp := lib.Get(); "
        "    var r: A.R? := null; "
        "    if sp.Contains(r) then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(diags != "LIBRARY-COMPILE-FAILED");
    BEZEL_EXPECT_TRUE(diags.find(kNullableDiag) == std::string::npos);
    BEZEL_EXPECT_TRUE(diags.find("IEnumerable<T>.Contains") ==
                      std::string::npos);
}

// #2593 / stdlib §4.14: the same receiver with `IndexOf(value: T?)` — the
// second nullable-parameter search member of the class-`T` span form.
BEZEL_TEST(module_imported_builtin_generic_2593,
           imported_class_t_span_indexof_accepts_nullable_argument) {
    std::string diags = diagnosticsOfCrossModuleImport(
        kLib,
        "module B; "
        "import A; "
        "public class Main "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<A.R>(1); "
        "    using var lib := new A.Lib(buf); "
        "    var sp := lib.Get(); "
        "    var r: A.R? := null; "
        "    return sp.IndexOf(r); "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(diags != "LIBRARY-COMPILE-FAILED");
    BEZEL_EXPECT_TRUE(diags.find(kNullableDiag) == std::string::npos);
    BEZEL_EXPECT_TRUE(diags.find("IEnumerable<T>.IndexOf") ==
                      std::string::npos);
}
