// fix #2589 (§23.1/§23/§2.8.2/§28.9, v0.236): directly invoking a
// function-typed auto-PROPERTY — `obj.P(args)` where `P` is declared
// `property P: (int) -> int := lambda(x) => x + 1;` — silently miscompiled,
// returning 0 (the #2477 zero-init fallback) instead of the call result, for
// BOTH a local (single-module) AND an imported class. Binding the property to
// a local first (`var g := obj.P; g(args)`) was a complete workaround and
// produced the correct value, so the property READ (the auto-property backing-
// field load) was always correct — only the direct callee dispatch was broken.
//
// ROOT CAUSE: `Emitter::lowerCall` (LlvmBackend_Calls2.cpp) had closure-
// dispatch arms for `ParameterSymbol`, `LocalSymbol`, and `FieldSymbol`
// callees (#2300 added the imported-field type resolution to the FieldSymbol
// arm), but NO `PropertySymbol` arm at all. `obj.P(args)` resolves `calleeSym`
// to a `PropertySymbol`, missed every dispatch arm, and fell through to the
// #2477 "codegen cannot dispatch member 'P'" zero-init fallback — silently
// returning 0 (garbage) instead of the call result. This is a DIFFERENT root
// cause from #2300 (which is the FieldSymbol arm's imported-type resolution):
// the property defect has no arm at all, so it bites locally too (single-
// module), not just across --import.
//
// THE FIX (this commit): a new PropertySymbol callee arm in lowerCall,
// mirroring the FieldSymbol arm — resolve the property's function TypeRef
// from the PropertyDecl's declared type (or, for an IMPORTED property with no
// AST decl, from `ctx_.importedMemberType`), then read the closure pointer via
// `lowerExpr(*call.callee)` (which dispatches the property GET) and
// `emitClosureCall`. The imported-type fallback was gated on a CLASS receiver,
// mirroring #2300's record gate: an imported RECORD function-typed property
// read shared the separate deeper defect #2590 (bad cross-module closure
// pointer), so routing it through emitClosureCall would only have converted
// its silent zero-init into a segfault; the LOCAL record case (sound read
// path) is covered by the `pd->type` branch.
//
// ⚠ THAT CARVE-OUT IS RETIRED (fix #2590, §2.8.2/§9.2/§23.1/§28.9, v0.306).
// The imported-record field-initializer replay (#2874/#3671/#3723) now lands a
// real closure pointer at the consumer's `new` site, so `Record` joins `Class`
// and `Interface` in the single shared
// `importedMemberTypeAdmitsClosureDispatch` predicate that replaced this arm's
// and #2300's separate allowlists. Nothing in THIS file's oracles changes —
// no row here uses a record receiver — but the rationale above no longer
// describes the code, and the record rows now live in
// tests/module/xmod_imported_record_func_member_invoke_2590_test.cpp.
//
// TEST GRID — every case asserts the VALUE (not just "compiles"), since the
// defect was a silent wrong-value miscompile:
//   T1 local class auto-property direct invoke        — the fix (0→11)
//   T2 IMPORTED class auto-property direct invoke     — the fix (0→11)
//   T3 field-invoke (#2300 control)                   — unchanged (11 either way)
//   T4 value-property read + auto-prop invoke compose — value-prop control
//                                                       (5→16; proves obj.N read
//                                                        is unaffected and the
//                                                        invoke now composes)
//   T5 local-bind on an auto-property (regression)    — unchanged (11 either way)
//   T6 IMPORTED interface function-typed property     — the fix (0→11); the
//     direct invoke                                         imported-type fallback
//                                                           was gated on Class ||
//                                                           Interface (Record was
//                                                           excluded — #2590 ABI
//                                                           defect segfaulted;
//                                                           since #2590 landed,
//                                                           Record is admitted
//                                                           too — see the ⚠ note
//                                                           above).

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2589 {
    std::string path;
    explicit TempFile2589(std::string p) : path(std::move(p)) {}
    ~TempFile2589() { std::remove(path.c_str()); }
};

void writeSource2589(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2589(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it in a
// SEPARATE process (fork-isolated per tests/README.md's AOT harness
// discipline), return its exit code (-1 on compile/link/run failure, -2 if
// the dependency itself could not be produced). Mirrors #2300's own
// compileImportAndRun helper. NB: exit code is WEXITSTATUS-masked to 8 bits,
// so each case's Main returns a distinct value in 0..255.
int compileImportAndRun2589(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2589_");
    TempFile2589 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2589 aObj((dir / (stem + "_a.o")).string());
    writeSource2589(aSrc.path, libSrc);
    if (!compileToObj2589(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2589 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2589 bExe((dir / stem).string());
    writeSource2589(bSrc.path, appSrc);
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

// Single-module compile -> exe -> run, returning the exit code. -1 on
// compile/link/run failure.
int compileAndRun2589(const std::string& src) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2589s_");
    TempFile2589 srcFile((dir / (stem + ".bzl")).string());
    TempFile2589 exe((dir / stem).string());
    writeSource2589(srcFile.path, src);
    if (!driver.compileFile(srcFile.path, exe.path, {}).success) return -1;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — the #2589 core: directly invoke a function-typed auto-PROPERTY on a
// LOCAL (single-module) class. Pre-fix: the #2477 "cannot dispatch member
// 'P'" zero-init fallback fired and Main returned 0. Post-fix: 11. 11 cannot
// arise from the zero-init default (0), so a passing assertion is genuine
// proof the property-invoke dispatched the closure, not a coincidence.
// ===========================================================================
BEZEL_TEST(xmod_func_property_invoke_2589,
           local_class_function_typed_auto_property_direct_invoke_23_1) {
    int r = compileAndRun2589(
        "module Local2589T1; "
        "public class Box2589T1 "
        "  public property P: (int) -> int := lambda(x: int) => x + 1; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Box2589T1; "
        "public class Program2589T1 "
        "  public static method Main(): int "
        "    using var b := new Box2589T1(); "
        "    var d := b.P(10); "
        "    return d; "
        "  end Main; "
        "end Program2589T1;");
    BEZEL_EXPECT_EQ(r, 11);
}

// ===========================================================================
// T2 — the #2589 IMPORTED form: directly invoke a function-typed auto-
// property on a class reached through `--import`. The issue's second scope
// claim ("local AND imported"). Pre-fix: 0 (same #2477 fallback; the
// PropertySymbol arm was absent entirely, so --import made no difference).
// Post-fix: 11.
// ===========================================================================
BEZEL_TEST(xmod_func_property_invoke_2589,
           imported_class_function_typed_auto_property_direct_invoke_2_8_2) {
    int r = compileImportAndRun2589(
        "module LibX2589T2; "
        "public class Holder2589T2 "
        "  public property P: (int) -> int := lambda(x: int) => x + 1; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Holder2589T2;",
        "module App2589T2; import LibX2589T2; "
        "public class Program2589T2 "
        "  public static method Main(): int "
        "    using var h := new Holder2589T2(); "
        "    var d := h.P(10); "
        "    return d; "
        "  end Main; "
        "end Program2589T2;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 11);
}

// ===========================================================================
// T3 — REGRESSION GUARD (#2300 control): directly invoke a function-typed
// FIELD (`obj.F(args)`). #2300 fixed the field-invoke and this case MUST keep
// passing — it proves the new PropertySymbol arm did not regress the
// FieldSymbol arm that fires before it for a field-typed callee. Passes both
// pre- and post-fix (that is the point of a regression guard).
// ===========================================================================
BEZEL_TEST(xmod_func_property_invoke_2589,
           function_typed_field_direct_invoke_2300_control_2_8_2) {
    int r = compileAndRun2589(
        "module Local2589T3; "
        "public class Box2589T3 "
        "  public F: (int) -> int := lambda(x: int) => x + 1; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Box2589T3; "
        "public class Program2589T3 "
        "  public static method Main(): int "
        "    using var b := new Box2589T3(); "
        "    var d := b.F(10); "
        "    return d; "
        "  end Main; "
        "end Program2589T3;");
    BEZEL_EXPECT_EQ(r, 11);
}

// ===========================================================================
// T4 — VALUE-PROPERTY control + compose: `obj.P(10) + obj.N` where P is a
// function-typed auto-property and N is a plain int auto-property (value
// read). Proves (a) a non-function-typed property read (`obj.N`) is unaffected
// by the new arm (it is not a CallExpr, so lowerCall is never entered for it;
// the value read still composes arithmetically), and (b) the function-typed
// property invoke's result composes with another value. Pre-fix: 0 + 5 = 5
// (the invoke degraded to zero-init, the value-property read was already
// correct). Post-fix: 11 + 5 = 16.
// ===========================================================================
BEZEL_TEST(xmod_func_property_invoke_2589,
           value_property_read_composed_with_auto_property_invoke_23) {
    int r = compileAndRun2589(
        "module Local2589T4; "
        "public class Box2589T4 "
        "  public property P: (int) -> int := lambda(x: int) => x + 1; "
        "  public property N: int := 5; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Box2589T4; "
        "public class Program2589T4 "
        "  public static method Main(): int "
        "    using var b := new Box2589T4(); "
        "    var d := b.P(10) + b.N; "
        "    return d; "
        "  end Main; "
        "end Program2589T4;");
    BEZEL_EXPECT_EQ(r, 16);
}

// ===========================================================================
// T5 — REGRESSION GUARD: the local-bind workaround (`var g := obj.P; g(args)`)
// worked even BEFORE this fix (it dispatches through the LocalSymbol arm,
// where the property READ supplies the closure pointer and the local's
// declType supplies the function type). This case MUST keep passing — it
// proves the fix did not break the already-working local-bind path on a
// function-typed auto-property. Passes both pre- and post-fix.
// ===========================================================================
BEZEL_TEST(xmod_func_property_invoke_2589,
           local_bind_function_typed_auto_property_invoke_23_1) {
    int r = compileAndRun2589(
        "module Local2589T5; "
        "public class Box2589T5 "
        "  public property P: (int) -> int := lambda(x: int) => x + 1; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Box2589T5; "
        "public class Program2589T5 "
        "  public static method Main(): int "
        "    using var b := new Box2589T5(); "
        "    var g := b.P; "
        "    var e := g(10); "
        "    return e; "
        "  end Main; "
        "end Program2589T5;");
    BEZEL_EXPECT_EQ(r, 11);
}

// ===========================================================================
// T6 — IMPORTED interface function-typed property direct invoke: `i.F(10)`
// where `i: IFunc` is reached through `--import`. The imported-type fallback
// in the new PropertySymbol arm was gated on Class || Interface (Record was
// excluded — its cross-module closure READ segfaulted, the #2590 ABI defect;
// #2590's fix retired that exclusion — see this file's header ⚠ note).
// Pre-fix: 0 (no PropertySymbol arm at all). Post-fix: 11. Proves the
// imported interface property read (itable dispatch) yields a sound closure
// pointer that emitClosureCall can dispatch.
// ===========================================================================
BEZEL_TEST(xmod_func_property_invoke_2589,
           imported_interface_function_typed_property_direct_invoke_2_8_2) {
    int r = compileImportAndRun2589(
        "module LibIface2589T6; "
        "public interface IFunc2589T6 "
        "  property F: (int) -> int; "
        "end IFunc2589T6; "
        "public class Impl2589T6 : IFunc2589T6 "
        "  public property F: (int) -> int := lambda(x: int) => x + 1; "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Impl2589T6;",
        "module AppIface2589T6; import LibIface2589T6; "
        "public class Program2589T6 "
        "  public static method Main(): int "
        "    using var i: IFunc2589T6 := new Impl2589T6(); "
        "    var d := i.F(10); "
        "    return d; "
        "  end Main; "
        "end Program2589T6;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 11);
}

}  // namespace bezel::module
