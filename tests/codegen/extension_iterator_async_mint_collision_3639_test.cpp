// Codegen — fix #3639: extension-method iterator / async state-machine mint
// prefixes (`bz$iter$…`, `bz$async$…`, `bz$asynciter$…`) must be qualified by
// the receiver type, not the (null) `ms->parent`.
//
// Background. fix #3536 qualified those three prefixes with
// `mangledTypeName(ms->parent)`, closing the class-method collision. For an
// EXTENSION method (§26) `ms->parent` is null by design (`Emitter::isExtensionMethod`'s
// doc comment, LlvmBackend_impl.hpp ~L6098) and the receiver type lives on
// `ms->extensionDecl->targetType`. The post-#3536 prefix was therefore
// `bz$iter$$<name>$*` for every extension iterator — two extensions with the
// same method name on different receiver types both minted the identical
// family. With the existing per-TU InternalLinkage the linker did not error
// (each TU keeps its own copy), but a future existence-check cache guard at
// the mint site (the shape #3477 already applied to eleven sibling mints in
// LlvmBackend_Assign.cpp) would silently hand the second extension a private
// `.1`-style copy of the first extension's MoveNext / Current / Dispose over
// its own differently-shaped frame. The OUTER function name (`functionName`
// at LlvmBackend_Setup.cpp) was already correct for this case; the gap was
// purely the three state-machine mint sites.
//
// The fix #3639 applied uniformly across all three emit sites
// (emitIteratorMethod / emitAsyncMethod / emitAsyncIteratorMethod) replaces
// `mangledTypeName(ms->parent)` with an identical lambda computing
// `mangledTypeName(targetType->declaration)` for an extension, falling back
// to the bare AST type-name parts for a primitive target (`extension int`,
// `extension string`) — mirroring `functionName`'s own isExtensionMethod
// branch.
//
// This regression test asserts the linker-visible consequence of the fix:
// an extension iterator method's INTERNAL state-machine symbols carry the
// receiver type's name as the qualifier segment between `bz$iter$` and
// `$<method>$`. Pre-fix the qualifier segment is empty (collision class the
// issue names); post-fix it is the receiver type's name. Same drill for the
// async variants.
//
// Source: spec §28.3 / §29.3 / §30.1 / §30.7 (v0.286, the loader for the
// state-mint namespace), #3536, #3639, #1488.

#include "driver/Driver.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

struct TempFile3639 {
    std::string path;
    explicit TempFile3639(std::string p) : path(std::move(p)) {}
    ~TempFile3639() { std::remove(path.c_str()); }
};

void writeSource3639(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::string slurpFile3639(const std::string& path, std::size_t cap = 1u << 18) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string out;
    out.reserve(std::min<std::size_t>(cap, 1u << 16));
    char buf[8192];
    bool truncated = false;
    while (in) {
        in.read(buf, sizeof(buf));
        auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;
        std::size_t room = out.size() < cap ? cap - out.size() : 0;
        if (got <= room) {
            out.append(buf, got);
        } else {
            out.append(buf, room);
            truncated = true;
            break;
        }
    }
    if (truncated) out += "\n[...output truncated at 256 KiB...]";
    return out;
}

// Symbol extractor. The InternalLinkage `bz$…` symbols do not appear in `nm`
// after linking, but they DO survive in the .o symtab (marked `t` for
// text-section internal) and in `nm`'s default text dump. We need a
// linker-aware answer — not just one module's symbols — so the test emits
// the IR with BEZEL_DUMP_IR (which LlvmBackend.cpp:33 prints to llvm::errs
// when that env var is set) and greps the IR lines for `define internal`
// and `internal constant` declarations.
struct SymbolSet3639 {
    std::string irText;
    bool hasMoveNextOn(const std::string& qualifier,
                       const std::string& method) const {
        // accept exactly: bz$iter$<qualifier>$<method>$MoveNext
        std::string needle = "@\"bz$iter$" + qualifier + "$" + method + "$MoveNext\"";
        std::string needleDef = "define internal i1 " + needle + "(";
        return irText.find(needleDef) != std::string::npos;
    }
    bool hasAsyncOn(const std::string& qualifier,
                    const std::string& method) const {
        // bz$async$<qualifier>$<method>$Resume — async methods mint Resume (not MoveNext).
        std::string needle = "@\"bz$async$" + qualifier + "$" + method + "$Resume\"";
        std::string needleDef = "define internal void " + needle + "(";
        return irText.find(needleDef) != std::string::npos;
    }
    bool hasAsyncIterOn(const std::string& qualifier,
                        const std::string& method) const {
        // bz$asynciter$<qualifier>$<method>$Resume — async iterators mint Resume.
        std::string needle = "@\"bz$asynciter$" + qualifier + "$" + method + "$Resume\"";
        std::string needleDef = "define internal void " + needle + "(";
        return irText.find(needleDef) != std::string::npos;
    }
};

// In-process compile with BEZEL_DUMP_IR=1 — capture llvm::errs() output by
// dup2-ing fd 2 to a temp file for the duration of the compile. llvm::errs()
// writes via raw write() to fd 2 directly (no C stdio buffering), so freopen
// on stderr wouldn't help; dup2 at the OS level does. Save/restore fd 2
// around the compile to avoid bleeding capture into the harness stderr.
static std::atomic<unsigned> g_3639_seq{0};
SymbolSet3639 compileAndCaptureIr3639(const std::string& src) {
    SymbolSet3639 r;
    const unsigned seq = g_3639_seq.fetch_add(1);
    auto dir = fs::temp_directory_path() /
        ("bezel_3639_" + std::to_string(::getpid()) + "_" +
         std::to_string(seq));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string srcPath = (dir / "prog.bzl").string();
    const std::string objPath = (dir / "prog.o").string();
    const std::string llPath = (dir / "prog.ll").string();
    writeSource3639(srcPath, src);
    int savedStderr = ::dup(2);
    int llFd = ::open(llPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (llFd >= 0) {
        ::dup2(llFd, 2);
        ::close(llFd);
    }
    ::setenv("BEZEL_DUMP_IR", "1", 1);
    bezel::Driver driver;
    auto result = driver.compileObject(srcPath, objPath);
    ::unsetenv("BEZEL_DUMP_IR");
    // Sync stdio + llvm::errs' internal buffer to disk before the restore
    // unhooks fd 2 — llvm's raw_ostream sometimes buffers across the
    // m->print() boundary and the buffered bytes are then lost on the
    // restore. fflush(NULL) flushes every open C FILE*.
    std::fflush(nullptr);
    if (savedStderr >= 0) {
        ::dup2(savedStderr, 2);
        ::close(savedStderr);
    }
    if (result.success) {
        r.irText = slurpFile3639(llPath);
    }
    fs::remove_all(dir);
    return r;
}

}  // namespace

// ===========================================================================
// T1 — Iterators. An extension iterator method on a PRIMITIVE receiver
// (`extension int`) should mint `bz$iter$int$MethodName$MoveNext`, not the
// un-qualified `bz$iter$$MethodName$MoveNext` the pre-fix code produced.
// §26.2 primitive extension / §29.3 iterator methods, v0.286.
//
// Pre-fix this test fails: the only `MoveNext` definition in the dumped IR
// matches the un-qualified pattern, the qualified pattern is absent.
// Post-fix the qualified pattern is present and the un-qualified variant
// for this `MethodName` is absent — confirming the receiver-type
// qualification is what the emitIteratorMethod lambda adds.
// ===========================================================================
BEZEL_TEST(codegen_extension_mint_3639, iter_extension_int_qualifies_prefix) {
    auto sym = compileAndCaptureIr3639(
        "module M; "
        "import System.Collections.Generic; "
        "extension int "
        "  public method YieldInts(): IEnumerable<int> "
        "    yield return 1; "
        "    yield return 2; "
        "  end YieldInts; "
        "end;");
    BEZEL_EXPECT_FALSE(sym.irText.empty());  // IR was captured
    if (sym.irText.empty()) return;
    // Qualifier segment present
    BEZEL_EXPECT_TRUE(sym.hasMoveNextOn("int", "YieldInts"));
    // Un-qualified variant absent — proves the fix's qualifier is what
    // made the difference (mangledTypeName(nullptr) would have minted "").
    BEZEL_EXPECT_FALSE(sym.hasMoveNextOn("", "YieldInts"));
}

// ===========================================================================
// T2 — Async method (§30.1). An async extension method on a CLASS
// receiver mints `bz$async$<Type>$<Method>$Resume` — the receiver type
// resolved via `ctx_.resolvedType(*targetType)->declaration` is the class's
// `mangledTypeName`. Pre-fix, the qualifier is empty.
// ===========================================================================
BEZEL_TEST(codegen_extension_mint_3639, async_extension_class_qualifies_prefix) {
    auto sym = compileAndCaptureIr3639(
        "module M; "
        "public class Box "
        "  public _x: int := 0; "
        "  public constructor(v: int) _x := v; end constructor; "
        "end Box; "
        "extension Box "
        "  public async method SlowAsync(): Task<int> => this._x + 1; "
        "end;");
    BEZEL_EXPECT_FALSE(sym.irText.empty());
    if (sym.irText.empty()) return;
    // `Box` is the `mangledTypeName(ts->declaration)` — module-qualified per
    // #1488 (class `Box` in `module M` → `M.Box`). Either spelling proves
    // the qualifier is non-empty (the discriminator vs the empty pre-fix
    // case); the strict match against `M.Box` is the cross-module-stable
    // form #1488 already pins at the OTHER mint sites.
    BEZEL_EXPECT_TRUE(sym.hasAsyncOn("M.Box", "SlowAsync"));
    BEZEL_EXPECT_FALSE(sym.hasAsyncOn("", "SlowAsync"));
}

// ===========================================================================
// T3 — Async iterator (§30.7). An extension that yields awaits mints
// `bz$asynciter$<ReceiverType>$<Method>$Resume`. Cross-checks that the
// SAME qualification lambda is applied at the third emit site, not just
// the iterator and async-method sites. Pre-fix this is also unqualified.
// ===========================================================================
BEZEL_TEST(codegen_extension_mint_3639, asynciter_extension_int_qualifies_prefix) {
    auto sym = compileAndCaptureIr3639(
        "module M; "
        "import System.Collections.Generic; "
        "extension int "
        "  public async method AsyncInts(): IAsyncEnumerable<int> "
        "    yield return this; "
        "  end AsyncInts; "
        "end;");
    BEZEL_EXPECT_FALSE(sym.irText.empty());
    if (sym.irText.empty()) return;
    BEZEL_EXPECT_TRUE(sym.hasAsyncIterOn("int", "AsyncInts"));
    BEZEL_EXPECT_FALSE(sym.hasAsyncIterOn("", "AsyncInts"));
}

// ===========================================================================
// T4 — Ordinary class iterator method (regression-guard for the #3536 path).
// The fix MUST NOT widen: `bz$iter$<Class>$<Method>$MoveNext` for an
// instance method on a class with no enclosing type path keeps its class
// qualifier exactly as #3536 specified. This row pins that the lambda
// still emits the class-qualified prefix when `isExtensionMethod(ms)` is
// false — catching a hypothetical future regression where the lambda
// over-broadens its branch.
// ===========================================================================
BEZEL_TEST(codegen_extension_mint_3639, class_iterator_keeps_parent_qualifier) {
    auto sym = compileAndCaptureIr3639(
        "module M; "
        "import System.Collections.Generic; "
        "public class Stream "
        "  public method YieldInts(): IEnumerable<int> "
        "    yield return 1; "
        "  end YieldInts; "
        "end Stream;");
    BEZEL_EXPECT_FALSE(sym.irText.empty());
    if (sym.irText.empty()) return;
    // Module-qualified name `M.Stream` — the cross-module-stable form
    // `mangledTypeName` already emits (#1488). Pre-fix AND post-fix this
    // works identically; the row pins that the fix's lambda does not
    // OVER-apply its extension branch and lose the class-mangling path
    // for ordinary class methods.
    BEZEL_EXPECT_TRUE(sym.hasMoveNextOn("M.Stream", "YieldInts"));
    // The empty-qualifier match would mean the fix accidentally lost the
    // class-mangling path; the lambda must short-circuit to mangledTypeName
    // for non-extension methods.
    BEZEL_EXPECT_FALSE(sym.hasMoveNextOn("", "YieldInts"));
}
