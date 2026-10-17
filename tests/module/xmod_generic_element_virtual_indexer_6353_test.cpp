// fix #6353 (§23.4 / §14.1 / §12.1 / §2.8.3 rule 4, v0.338): a VIRTUAL indexer
// on an IMPORTED GENERIC base, read through a base-typed receiver, dispatches
// through a vtable slot the ERASED element type — so a `string`-valued indexer
// is called through an `i32` prototype and its result is silently garbage.
//
// THE DEFECT. §23.4 (v0.338): "Indexers may be overloaded by parameter type
// list. They participate in `virtual`/`override`/`sealed`/`abstract` like
// methods." A `virtual property [i: int]: T` on an imported generic base is
// therefore a genuine §12 dynamic-dispatch site, and §14.1 makes the receiver's
// `LIB.VB<string>` an INSTANTIATION of `LIB.VB<T>` by substituting `string`
// for `T`. §2.8.3 rule 4 puts one runtime identity per instantiation.
//
// The vtable SLOT holds the LOCAL override's accessor, emitted in the importing
// module against the SUBSTITUTED element (`[i: int]: string` → `ptr`). But
// `declareImportedIndexerGetters` (LlvmBackend_Setup.cpp) builds the
// `bz$<cls>$vt$idxget$<arity>$<slot>` dispatch wrapper's LLVM signature from
// `ctx_.importedMemberType(ps)` — the RECONSTRUCTED, UNSUBSTITUTED `T`, which
// lowers to the i32 erasure placeholder — and `lowerIndexerGet`'s `!pd` branch
// registers and calls exactly that wrapper. So the wrapper calls a
// `ptr`-returning function through an `i32`-returning prototype.
//
// LLVM's verifier does NOT catch this: a pointer and an i32 are both
// register-class integers at a call site, so there is no type mismatch to
// abort on. The callee's return register is simply re-read as an i32 — the
// caller gets the low 32 bits of a heap pointer. NO diagnostic, rc=0.
//
// MEASURED on this fix's parent, `bezelc -c lib.bzl` then
// `bezelc --import lib.o app.bzl` on the repro below: `b[0]` printed `2358800`
// — a heap pointer's low word — where `XMOD` was correct. The NON-GENERIC
// control (`class VB` with a concrete `[i: int]: string`) printed `XMOD`
// correctly, which is what isolates the GENERIC ELEMENT as the trigger.
//
// The related SHAPES, all measured on the same parent, and the reason this
// test has the rows it has:
//   * generic ELEMENT, get, `string`   → SILENT GARBAGE (this issue).
//   * generic ELEMENT, get, `int`      → correct by ACCIDENT: `int` lowers to
//     i32, identical to the erasure placeholder, so the two ABIs coincide.
//   * generic ELEMENT, SET, `string`   → LOUD: an LLVM verifier abort, "Call
//     parameter type does not match function signature! ptr @.bz.str.55" —
//     a passed pointer where the prototype declares an i32.
//   * generic ELEMENT, set, `int`      → correct (same accident as the get).
//   * generic KEY, get, `string` index → correctly REJECTED at sema by #2955's
//     gate (ExprTypecheck_Exprs.cpp), which examines `ps->indexerParams` ONLY
//     and so never covered the ELEMENT type. Its own diagnostic text names the
//     remaining work as "importer-side accessor monomorphization per
//     instantiation" — precisely what this fix does.
//   * generic KEY, get, `int` index    → correct by the same i32 accident.
//
// THE FIX. `emitImportedVirtualIndexerSubstWrapper` (LlvmBackend_Calls.cpp)
// emits a per-(ps, recvTy, get/set) `InternalLinkage` wrapper whose signature
// is built UNDER the receiver's §14.1 substitution and whose body is #2545's
// dispatch verbatim. Wired at all three faces: `lowerIndexerGet`,
// `lowerIndexerSet`, and `lowerCompoundIndexerAssign` (which needs BOTH — the
// set for the write, the get for the read-back).
//
// The gate is narrow by design, and every clause of it is exercised below or
// by an adjacent row: the accessor must be IMPORTED, VIRTUAL, have a real
// vtable slot, live on a GENERIC parent, and the receiver must carry a
// provable substitution — AND the substituted signature must actually DIFFER
// from the erased one, the same "act only where the erasure moves the ABI"
// test #2955's sema gate uses. A `VB<int>` receiver is therefore left on the
// pre-existing, already-correct path rather than routed through a redundant
// wrapper.
//
// HARNESS NOTE. This file does not reuse `indexer_inherited_import_2449_test`'s
// `compileImportAndRun2449` verbatim because that helper stages its sources and
// objects in `std::filesystem::temp_directory_path()`, which is `/tmp` — a
// quota'd tmpfs this repo's lane protocol forbids for scratch. The mechanism is
// otherwise identical (fork-isolated `Driver::compileObject` for the library,
// `Driver::compileFile` with a `Dependency{metadataPath, objectPath}` for the
// app, `std::system` + `WEXITSTATUS` for the run) and is re-declared here
// against a caller-supplied directory.

#include <sys/wait.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "driver/Driver.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct ScratchFile6353 {
    std::string path;
    ~ScratchFile6353() { std::remove(path.c_str()); }
};

void writeSource6353(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `src` to a relocatable object at `obj`. Returns false on a front-end
// or backend error.
bool compileToObj6353(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// The scratch root for this test's sources, objects and executables. Never
// `temp_directory_path()` (/tmp is a quota'd tmpfs this repo forbids for
// scratch); `BEZEL_TEST_SCRATCH_DIR` lets a caller point at a lane directory,
// and the fallback is `scratch/` beside the build tree.
std::filesystem::path scratchDir6353() {
    if (const char* env = std::getenv("BEZEL_TEST_SCRATCH_DIR"))
        if (env[0] != '\0') {
            std::filesystem::create_directories(env);
            return std::filesystem::path(env);
        }
    std::filesystem::path p = "scratch/xmod6353";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

struct RunResult6353 {
    bool compiled = false;
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

// Compile `libSrc` to an object, compile `appSrc` (importing it) to an
// executable, and RUN the executable in a separate process (per tests/README's
// AOT harness discipline). Returns `compiled == false` if either compile
// failed — including the REJECT controls, which assert on that.
RunResult6353 compileImportAndRun6353(const std::string& libSrc,
                                      const std::string& appSrc) {
    namespace fs = std::filesystem;
    RunResult6353 out;
    auto dir = scratchDir6353();
    std::string stem = unique_stem("bz6353_");
    ScratchFile6353 aSrc((dir / (stem + "_a.bzl")).string());
    ScratchFile6353 aObj((dir / (stem + "_a.o")).string());
    writeSource6353(aSrc.path, libSrc);
    if (!compileToObj6353(aSrc.path, aObj.path)) return out;
    if (!std::ifstream(aObj.path).good()) return out;

    ScratchFile6353 bSrc((dir / (stem + "_b.bzl")).string());
    ScratchFile6353 bOut((dir / (stem + "_b.txt")).string());
    ScratchFile6353 bErr((dir / (stem + "_b.err")).string());
    writeSource6353(bSrc.path, appSrc);

    std::string exe = (dir / (stem + "_b.bin")).string();
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        out.compiled = driver.compileFile(bSrc.path, exe, deps).success;
    }
    if (!out.compiled) return out;

    // Capture stdout/stderr through files rather than a pipe: this program is
    // short-lived and deterministic, and a pipe needs a reader thread or a
    // second fork to avoid deadlocking on a full buffer.
    std::string cmd = exe + " >" + bOut.path + " 2>" + bErr.path;
    int status = std::system(cmd.c_str());
    out.exitCode =
        (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    std::ifstream outStream(bOut.path);
    out.stdoutText.assign(std::istreambuf_iterator<char>(outStream),
                          std::istreambuf_iterator<char>());
    std::ifstream errStream(bErr.path);
    out.stderrText.assign(std::istreambuf_iterator<char>(errStream),
                          std::istreambuf_iterator<char>());
    std::remove(exe.c_str());
    return out;
}

// §23.4 / §14.1 / §12.1 / §2.8.3 rule 4 / #6353 (v0.338): the issue's own
// two-module repro, unmodified. Pre-fix this compiles clean, runs with rc=0,
// and prints a heap pointer's low word instead of `XMOD` — the whole shape of
// the bug is that NOTHING goes wrong, so the assertion is on the VALUE. The
// exit code is asserted too (a wrong value is not an acceptable substitute for
// a crash; a crash would be a loud bug and this is not one).
BEZEL_TEST(xmod_generic_element_virtual_indexer_6353,
           imported_generic_element_get_returns_override_value) {
    const std::string libSrc =
        "module LIB; "
        "public class VB<T> "
        "  public virtual property [i: int]: T "
        "    get; "
        "  end; "
        "end VB;";
    const std::string appSrc =
        "module APP; import LIB; "
        "public class E: LIB.VB<string> "
        "  public override property [i: int]: string "
        "    get => \"XMOD\"; "
        "  end; "
        "end E; "
        "public class C "
        "  public static method Main(): int "
        "    var b: LIB.VB<string> := new E(); "
        "    Console.WriteLine(b[0]); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = compileImportAndRun6353(libSrc, appSrc);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("XMOD") != std::string::npos);
}

// §23.4 / #6353 (v0.338): the NON-GENERIC CONTROL, byte-identical to the row
// above except that `VB` declares a concrete `string` element. It passed
// pre-fix and must still pass — it is what makes the generic row's failure a
// GENERIC-ELEMENT failure rather than "cross-module virtual indexers were
// never right" (which would make this a different, larger issue).
BEZEL_TEST(xmod_generic_element_virtual_indexer_6353,
           nongeneric_element_get_control_still_correct) {
    const std::string libSrc =
        "module LIB; "
        "public class VB "
        "  public virtual property [i: int]: string "
        "    get; "
        "  end; "
        "end VB;";
    const std::string appSrc =
        "module APP; import LIB; "
        "public class E: LIB.VB "
        "  public override property [i: int]: string "
        "    get => \"XMOD\"; "
        "  end; "
        "end E; "
        "public class C "
        "  public static method Main(): int "
        "    var b: LIB.VB := new E(); "
        "    Console.WriteLine(b[0]); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = compileImportAndRun6353(libSrc, appSrc);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("XMOD") != std::string::npos);
}

// §23.4 / §14.1 / #6353 (v0.338): the `int` ELEMENT twin — the shape whose
// correctness pre-fix was an ACCIDENT of the erasure placeholder being i32.
// This row's value is that it proves the fix's "only act where the erasure
// moves the ABI" gate does not BREAK the coincidentally-correct case: `int`
// still routes through the pre-existing wrapper (the substituted signature
// equals the erased one, so `emitImportedVirtualIndexerSubstWrapper` returns
// nullptr) and must keep working.
BEZEL_TEST(xmod_generic_element_virtual_indexer_6353,
           generic_element_int_get_control_still_correct) {
    const std::string libSrc =
        "module LIB; "
        "public class VB<T> "
        "  public virtual property [i: int]: T "
        "    get; "
        "  end; "
        "end VB;";
    const std::string appSrc =
        "module APP; import LIB; "
        "public class E: LIB.VB<int> "
        "  public override property [i: int]: int "
        "    get => 77; "
        "  end; "
        "end E; "
        "public class C "
        "  public static method Main(): int "
        "    var b: LIB.VB<int> := new E(); "
        "    return b[0]; "
        "  end Main; "
        "end C;";
    auto r = compileImportAndRun6353(libSrc, appSrc);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.exitCode == 77);
}

// §23.4 / §14.1 / #6353 (v0.338): the SETTER face. Pre-fix this did not
// silently misbehave — it aborted at LLVM verification with "Call parameter
// type does not match function signature! ptr @.bz.str.55" (a passed string
// pointer against an i32-declared parameter), i.e. it was LOUD. It is here
// because the fix must cover the write path too, and because a setter whose
// getter was fixed while the setter was not would be a strictly worse program
// than either: it would store into the override correctly and then read back
// garbage. The value is stored and READ BACK so both directions are exercised.
BEZEL_TEST(xmod_generic_element_virtual_indexer_6353,
           imported_generic_element_set_then_get_roundtrips) {
    const std::string libSrc =
        "module LIB; "
        "public class VB<T> "
        "  public virtual property [i: int]: T "
        "    get; "
        "    set; "
        "  end; "
        "end VB;";
    const std::string appSrc =
        "module APP; import LIB; "
        "public class E: LIB.VB<string> "
        "  public property _v: string := \"\"; "
        "  public override property [i: int]: string "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end E; "
        "public class C "
        "  public static method Main(): int "
        "    var b: LIB.VB<string> := new E(); "
        "    b[0] := \"STORED\"; "
        "    Console.WriteLine(b[0]); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = compileImportAndRun6353(libSrc, appSrc);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("STORED") != std::string::npos);
}

// §23.4 / §14.1 / #6353 (v0.338): the COMPOUND-ASSIGN face, which needs BOTH
// substituted entry points at once — the getter for the read-back and the
// setter for the write. Pre-fix the setter half aborted at verification; with
// only the getter substituted this row would still abort. `5 + 10` makes the
// expected result `15`, which distinguishes a genuinely executed compound from
// either a no-op or a mis-dispatched one.
BEZEL_TEST(xmod_generic_element_virtual_indexer_6353,
           imported_generic_element_compound_assign_roundtrips) {
    const std::string libSrc =
        "module LIB; "
        "public class VB<T> "
        "  public virtual property [i: int]: T "
        "    get; "
        "    set; "
        "  end; "
        "end VB;";
    const std::string appSrc =
        "module APP; import LIB; "
        "public class E: LIB.VB<string> "
        "  public property _v: string := \"5\"; "
        "  public override property [i: int]: string "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end E; "
        "public class C "
        "  public static method Main(): int "
        "    var b: LIB.VB<string> := new E(); "
        "    b[0] += \"X\"; "
        "    Console.WriteLine(b[0]); "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = compileImportAndRun6353(libSrc, appSrc);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_TRUE(r.exitCode == 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("5X") != std::string::npos);
}

}  // namespace
}  // namespace bezel::module