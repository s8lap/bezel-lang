// Spec-derived regression test for issue #5009 — an imported generic method
// whose body calls a member its `where T: IFace` bound provides was compiled
// only as the defining module's i32-placeholder template, so every call that
// reached it returned a silent zero.
//
// spec §14.1 "Generic methods" (a generic method's body is monomorphized per
// instantiation) / §14.2 "Generic constraints" (a type bound admits calls to
// the bound's members on a type-parameter-typed value; `new()` admits
// `new T()`) / §2.8.3 rule 2 (an imported artifact must not alter an
// instantiation's semantics), v0.307.
//
// ── THE GAP ────────────────────────────────────────────────────────────────
//
// A generic method on a NON-generic class is compiled once in its defining
// module against the i32 type-parameter placeholder. `v.Area()` on a
// placeholder-typed `v` cannot be lowered there, so the template body degrades
// to a zero return with the INTERNAL #2491 note (`new T()` likewise, #2477).
// The same-module program never calls that template — it monomorphizes — but
// the exporter shipped no body for this shape (#4600 opened the body-shipping
// path for RETURN-ONLY type parameters only), so an importer had nothing to
// monomorphize and called the template:
//
//   * `Gen<T>(v: T): int where T: IShape` with a class / record / string-
//     carrying argument: the #1943 argument-width gate rejected it (the
//     argument is not i32), so the constraint-dispatch shape could not be
//     called across a module boundary at all;
//   * `Mk<T>(): int where T: IShape, new()` — no type-parameter-typed
//     argument, so #1943 had nothing to compare and the call ran the
//     template: exit 0, where the byte-identical single-module program
//     returns 7. SILENT WRONG CODE.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// Export side (`ModuleMetadataBuilder.cpp`, `isConstraintUsingGenericMethod`):
// ship `methodBodyText` for a generic method whose own type parameter carries
// a `new()` bound or a TYPE bound declared in the same module. Import side
// (`ModuleLoader.cpp`, `parseAndAttachMethodBody`):
//   * a member of ANY public sibling type of the exporting module (and the
//     sibling type itself, as a qualifier) is carried into the body-symbol
//     remap by identity. That covers the bound's `v.Area()` and also
//     `Helper.Bonus()`, `b.V`, `b.Twice()` and `Consts.K`;
//   * on this route the reconstruction FAILS CLOSED. If any in-body reference
//     to the reconstructed type, one of its members, or a parameter is left
//     unbound, the body is not attached and the call keeps the #1943 clean
//     reject.
//   * a body on this route that contains a lambda, a function-valued local
//     or parameter, or any generic METHOD call is also not attached (binding
//     does not prove those lower);
// Codegen (`emitInlineGenericMethod`): the BACKSTOP. Any INTERNAL degrade
// reported while a constraint-route body is emitted becomes a located error
// at the call site, so a body that binds but does not lower is a clean
// reject, never a silent zero.
//
// The first version of this fix (539563d5) admitted only the bound's members.
// Every other sibling member was dropped, so programs that were previously
// rejected cleanly compiled and returned wrong values (T11-T13, found by the
// non-author review). T11-T16 pin that class.
// The second version (43854953) proved only BINDING. Bodies with a lambda or a
// nested generic call bound, did not lower, and returned 0 (T18-T23, found by
// a second non-author review); T24 pins the backstop for the shapes a
// syntactic refusal cannot enumerate.
// The third version (bd0405e6) shipped VIRTUAL generic bodies too. Those are
// lowered by `emitVirtualGenericDispatch`, which neither layer covers, and
// returned 0 or ran the base body for a derived receiver (T26-T28, found by a
// third non-author review). Virtual, override, abstract and interface methods
// are now kept off the route, and their call gets #1943's diagnostic on the
// vtable-slot path.
//
// Deliberately NOT covered, and pinned as unchanged (T9): a bound on a
// BUILTIN interface (`INumber<T>`, `IComparable<T>`) — the importer's fresh
// re-parse context re-synthesizes those interfaces as distinct symbols, so
// shipping that body turned a working call into a spurious §14.2 reject.
//
// ── WHAT EACH ROW PINS ─────────────────────────────────────────────────────
//
//   T1 ★ class argument, T inferred        -> 7   (pre-fix: #1943 reject)
//   T2 ★ class argument, explicit <Sq>     -> 7   (pre-fix: #1943 reject)
//   T3 ★ record (value kind) argument      -> 12  (pre-fix: #1943 reject)
//   T4 ★ string-carrying record argument   -> 5   (pre-fix: #1943 reject)
//   T5 ★ string-carrying class argument    -> 5   (pre-fix: #1943 reject)
//   T6 ★ interface-typed argument          -> 7   (pre-fix: #1943 reject)
//   T7 ★ THE SILENT ZERO. `Mk<Sq>()`, no T-typed argument -> 7 (pre-fix: 0)
//   T8   DIFFERENTIAL CONTROL: all of the above in ONE module -> 35
//   T9   NO-REGRESSION: builtin-bound `GenNum<int>` keeps the template -> 4
//   T10  NO-REGRESSION: #1943's `Count<T>(x: T): int where T: value` with a
//        `double` is still REJECTED with its specific diagnostic.
//   T11 ★ body calls a public sibling's static method, both inline and
//        through a local                -> 27 each (539563d5: 0 / 7)
//   T12 ★ body reads a sibling's field, calls its instance method, reads a
//        sibling's static readonly      -> 11+15+47 = 73 (539563d5: 0 0 0)
//   T13 ★ the importer's OWN conformer through T11's body -> 23 (539563d5: 3)
//   T14 ★ `new()`-only bodies calling a sibling's / the own class's static
//        method, with an importer-side homonym `HelperS5009` -> 27+37 = 64
//   T15 ★ a self-QUALIFIED static call `GS5009.PubHelp()` -> 37
//   T16 ★ FAIL CLOSED: a body naming a computed property the remap cannot
//        bind is REJECTED with #1943's diagnostic (without the fail-closed
//        check: exit 0 with INTERNAL #2491, measured)
//   T17  DIFFERENTIAL CONTROL for T11-T15 in ONE module -> 228
//   T18-T23 ★ bodies with a lambda (p8b / q1 / q3 / q4) or a member of a
//        nested generic call's result (p6c / q7) are REJECTED with #1943's
//        diagnostic (43854953: they bound, did not lower, and returned 0)
//   T24 ★ CODEGEN BACKSTOP: a body that binds but does not lower (a sibling
//        generic class at T) is REJECTED at the call site, not 0
//   T25  DIFFERENTIAL CONTROL for T18-T24 in ONE module -> 99
//   T26 ★ `virtual VBox<T>` (a sibling generic class at T) on its own class
//        is REJECTED with #1943's diagnostic (bd0405e6: 0, twin 107)
//   T27 ★ the same through a base-typed receiver on an override
//        (bd0405e6: 0, twin 207)
//   T28 ★ a plain `v.Area() + 100 + k` virtual body through a base-typed
//        receiver on an override (bd0405e6: 107 with NO warning, twin 207;
//        override selection across modules is #5031)
//   T29-T31 DIFFERENTIAL CONTROLS for T26-T28 in ONE module -> 107 / 207 / 207
//
// Every accept row asserts the constraint method's exact VALUE — "compiled"
// is what the pre-fix T7 also did.

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
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile5009 {
    std::string path;
    explicit TempFile5009(std::string p) : path(std::move(p)) {}
    ~TempFile5009() { std::remove(path.c_str()); }
};

void writeSource5009(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled5009 {
    bool depOk = true;  // false => dependency toolchain unavailable
    bool success = false;
    std::string err;
};

// Run one compile with fd 2 redirected to a file, so the driver's diagnostics
// are captured (the shape xmod_return_only_generic_method_4600 uses).
template <typename Fn>
std::string captureStderr5009(Fn&& fn) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz5009_err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return out;
}

// Build `libSrc` to an object, then compile (only) `appSrc` against it.
Compiled5009 compileImport5009(const std::string& libSrc,
                               const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5009_");
    TempFile5009 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5009 aObj((dir / (stem + "_a.o")).string());
    writeSource5009(aSrc.path, libSrc);
    Compiled5009 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile5009 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5009 bExe((dir / stem).string());
    writeSource5009(bSrc.path, appSrc);
    r.err = captureStderr5009([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        r.success =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                .success;
    });
    return r;
}

// Build the dependency, compile+link the importer, RUN it, return its exit
// code (-2 dependency unavailable, -1 compile/link/run failure).
// `compileFileForTest` / `runProgramForTest` (#4532) wire this runner into
// `gate.sh --full`: under BEZEL_AOT_FORCE_SANITIZE=address the importer is
// built with generated-code ASan while the row keeps asserting its VALUE.
int buildImportAndRun5009(const std::string& libSrc,
                          const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5009r_");
    TempFile5009 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile5009 libObjF((dir / (id + "_lib.o")).string());
    TempFile5009 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile5009 mainExeF((dir / id).string());
    writeSource5009(libSrcF.path, libSrc);
    writeSource5009(mainSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    if (!bezel::test::compileFileForTest(driver, mainSrcF.path, mainExeF.path,
                                         deps).success)
        return -1;
    int status = bezel::test::runProgramForTest(mainExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The one-module twin — no dependency anywhere.
int buildLocalAndRun5009(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5009l_");
    TempFile5009 srcF((dir / (id + ".bzl")).string());
    TempFile5009 exeF((dir / id).string());
    writeSource5009(srcF.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path).success)
        return -1;
    int status = bezel::test::runProgramForTest(exeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ---------------------------------------------------------------------------
// The dependency. The constraint interface, four conformers (a class, a
// record, a string-carrying record, a string-carrying class), the two
// constraint-using generic methods the fix ships, and two NO-REGRESSION
// siblings in the SAME module (the recognizer runs per METHOD):
//
//   G5009.Gen5009<T>(v: T): int where T: IShape5009        ships  (T1-T6,T8)
//   G5009.Mk5009<T>(): int where T: IShape5009, new()     ships  (T7,T8)
//   G5009.GenNum5009<T>(v: T): int where T: INumber<T>     builtin bound,
//                                                          template  (T9)
//   G5009.Count5009<T>(x: T): int where T: value          special-only,
//                                                          #1943    (T10)
// ---------------------------------------------------------------------------
const char* kDeclsBody5009 =
    "public interface IShape5009 "
    "  method Area(): int; "
    "end IShape5009; "
    "public class Sq5009: IShape5009 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end Sq5009; "
    "public record Rect5009(W: int, H: int): IShape5009 "
    "  public method Area(): int => W * H; "
    "end Rect5009; "
    "public record Named5009(Name: string): IShape5009 "
    "  public method Area(): int => Name.Length; "
    "end Named5009; "
    "public class Tag5009: IShape5009 "
    "  public Label: string; "
    "  public constructor(s: string) Label := s; end constructor; "
    "  public method Area(): int return Label.Length + 1; end Area; "
    "end Tag5009; "
    "public static class G5009 "
    "  public static method Gen5009<T>(v: T): int where T: IShape5009 "
    "    return v.Area(); "
    "  end Gen5009; "
    "  public static method Mk5009<T>(): int where T: IShape5009, new() "
    "    using var t := new T(); "
    "    return t.Area(); "
    "  end Mk5009; "
    "  public static method GenNum5009<T>(v: T): int where T: INumber<T> "
    "    return 4; "
    "  end GenNum5009; "
    "  public static method Count5009<T>(x: T): int where T: value "
    "    return 7; "
    "  end Count5009; "
    "end G5009; ";

std::string dep5009() {
    return std::string("module Dep5009; ") + kDeclsBody5009;
}

std::string app5009(const std::string& tag, const std::string& mainBody) {
    return "module App5009" + tag + "; import Dep5009; " +
           "class C5009 public static method Main(): int " + mainBody +
           " end Main; end C5009;";
}

std::string local5009(const std::string& tag, const std::string& mainBody) {
    return "module Loc5009" + tag + "; " + kDeclsBody5009 +
           "class C5009 public static method Main(): int " + mainBody +
           " end Main; end C5009;";
}

// Every conformer through both shipped methods: 7 + 12 + 5 + 4 + 7 (Mk) =
// 35 (`Tag5009("abc")` is 3 + 1).
// The differential total T8 compares.
const char* kAllBody5009 =
    "using var s := new Sq5009(); "
    "var r := new Rect5009(3, 4); "
    "var n := new Named5009(\"hello\"); "
    "using var t := new Tag5009(\"abc\"); "
    "return G5009.Gen5009(s) + G5009.Gen5009(r) + G5009.Gen5009(n) + "
    "G5009.Gen5009(t) + G5009.Mk5009<Sq5009>();";

// §14.1/§2.8.3, as the #1943 codegen gate emits it.
const char* kArgWidthReject5009 =
    "calling imported generic method `Count5009` with a type argument whose "
    "representation is not the compiled `int` (i32) placeholder";

// ---------------------------------------------------------------------------
// The second dependency, for T11-T17. Its constraint-using generic bodies
// reach PUBLIC SIBLINGS of their own class: a static method, a field, an
// instance method, a static readonly field, and the own class qualified by
// name. `AccS5009.GenPS5009` reads a COMPUTED property, which the importer's
// remap cannot bind, so it is the fail-closed row (T16).
// ---------------------------------------------------------------------------
const char* kSibDecls5009 =
    "public interface IShapeS5009 "
    "  method Area(): int; "
    "end IShapeS5009; "
    "public class SqS5009: IShapeS5009 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end SqS5009; "
    "public static class HelperS5009 "
    "  public static method Bonus(): int return 20; end Bonus; "
    "end HelperS5009; "
    "public class BoxS5009 "
    "  public V: int; "
    "  public constructor() V := 4; end constructor; "
    "  public method Twice(): int return V * 2; end Twice; "
    "end BoxS5009; "
    "public static class ConstsS5009 "
    "  public static readonly K: int := 40; "
    "end ConstsS5009; "
    "public class AccS5009 "
    "  public constructor() end constructor; "
    "  public property P: int => 5; "
    "  public method GenPS5009<T>(v: T): int where T: IShapeS5009 "
    "    return v.Area() + P; "
    "  end GenPS5009; "
    "end AccS5009; "
    "public static class GS5009 "
    "  public static method PubHelp(): int return 30; end PubHelp; "
    "  public static method Gen<T>(v: T): int where T: IShapeS5009 "
    "    return v.Area() + HelperS5009.Bonus(); "
    "  end Gen; "
    "  public static method Gen2<T>(v: T): int where T: IShapeS5009 "
    "    var x := HelperS5009.Bonus(); "
    "    return v.Area() + x; "
    "  end Gen2; "
    "  public static method GBox<T>(v: T): int where T: IShapeS5009 "
    "    using var b := new BoxS5009(); "
    "    return v.Area() + b.V; "
    "  end GBox; "
    "  public static method GTwice<T>(v: T): int where T: IShapeS5009 "
    "    using var b := new BoxS5009(); "
    "    return v.Area() + b.Twice(); "
    "  end GTwice; "
    "  public static method GK<T>(v: T): int where T: IShapeS5009 "
    "    return v.Area() + ConstsS5009.K; "
    "  end GK; "
    "  public static method QSelf<T>(v: T): int where T: IShapeS5009 "
    "    return v.Area() + GS5009.PubHelp(); "
    "  end QSelf; "
    "  public static method Mk<T>(): int where T: IShapeS5009, new() "
    "    using var t := new T(); "
    "    return t.Area() + HelperS5009.Bonus(); "
    "  end Mk; "
    "  public static method MkSelf<T>(): int where T: IShapeS5009, new() "
    "    using var t := new T(); "
    "    return t.Area() + PubHelp(); "
    "  end MkSelf; "
    "end GS5009; ";

std::string depSib5009() {
    return std::string("module DepS5009; ") + kSibDecls5009;
}

std::string appSib5009(const std::string& tag, const std::string& decls,
                       const std::string& mainBody) {
    return "module AppS5009" + tag + "; import DepS5009; " + decls +
           "class CS5009 public static method Main(): int " + mainBody +
           " end Main; end CS5009;";
}

// Every accepted sibling-reaching body: 27 (Gen) + 27 (Gen2) + 11 (GBox) +
// 15 (GTwice) + 47 (GK) + 37 (QSelf) + 27 (Mk) + 37 (MkSelf) = 228.
const char* kSibAllBody5009 =
    "using var s := new SqS5009(); "
    "return GS5009.Gen(s) + GS5009.Gen2(s) + GS5009.GBox(s) + "
    "GS5009.GTwice(s) + GS5009.GK(s) + GS5009.QSelf(s) + "
    "GS5009.Mk<SqS5009>() + GS5009.MkSelf<SqS5009>();";

const char* kFailClosedReject5009 =
    "calling imported generic method `GenPS5009` with a type argument whose "
    "representation is not the compiled `int` (i32) placeholder";

// ---------------------------------------------------------------------------
// The third dependency, for T18-T25 (the second non-author review of
// 43854953). Every body BINDS, but on 43854953 each one lowered a part of
// itself to nothing and the call returned 0 with only an INTERNAL #2491
// warning, where dcb17d57 rejected it cleanly with #1943. The single-module
// twins return the values in the comments.
// ---------------------------------------------------------------------------
const char* kLowDecls5009 =
    "public interface IShapeL5009 "
    "  method Area(): int; "
    "end IShapeL5009; "
    "public class SqL5009: IShapeL5009 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end SqL5009; "
    "public static class HelperL5009 "
    "  public static method Bonus(): int return 20; end Bonus; "
    "  public static method Id<U>(u: U): U return u; end Id; "
    "end HelperL5009; "
    "public class BoxL5009<U> "
    "  private _u: U; "
    "  public constructor(u: U) _u := u; end constructor; "
    "  public method Get(): U return _u; end Get; "
    "end BoxL5009; "
    "public static class GL5009 "
    "  public static method PubHelp(): int return 30; end PubHelp; "
    // p8b: a lambda capturing `v`, calling an own static. 7 + 30 = 37.
    "  public static method LamCapHelp<T>(v: T): int where T: IShapeL5009 "
    "    var g := lambda() => v.Area() + PubHelp(); "
    "    return g(); "
    "  end LamCapHelp; "
    // q1: a lambda capturing `v`. 7 + 100 = 107.
    "  public static method LamCap<T>(v: T): int where T: IShapeL5009 "
    "    var g := lambda() => v.Area(); "
    "    return g() + 100; "
    "  end LamCap; "
    // q3: a capture-free lambda calling a sibling's static. 20 + 7 = 27.
    "  public static method LamSib<T>(v: T): int where T: IShapeL5009 "
    "    var g := lambda() => HelperL5009.Bonus(); "
    "    return g() + v.Area(); "
    "  end LamSib; "
    // q4: a lambda whose parameter is T-typed. 7 + 100 = 107.
    "  public static method LamParam<T>(v: T): int where T: IShapeL5009 "
    "    var g := lambda(x: T) => x.Area(); "
    "    return g(v) + 100; "
    "  end LamParam; "
    // p6c: a member of a nested generic call's result, U inferred. 107.
    "  public static method IdInfer<T>(v: T): int where T: IShapeL5009 "
    "    return HelperL5009.Id(v).Area() + 100; "
    "  end IdInfer; "
    // q7: the same with an explicit `<T>`. 107.
    "  public static method IdExplicit<T>(v: T): int where T: IShapeL5009 "
    "    return HelperL5009.Id<T>(v).Area() + 100; "
    "  end IdExplicit; "
    // A sibling GENERIC CLASS instantiated at T. No lambda and no generic
    // METHOD call, so the pre-attach refusal does not see it; the codegen
    // backstop does. 7 + 100 = 107.
    "  public static method GenBox<T>(v: T): int where T: IShapeL5009 "
    "    var b := new BoxL5009<T>(v); "
    "    return b.Get().Area() + 100; "
    "  end GenBox; "
    "end GL5009; ";

std::string depLow5009() {
    return std::string("module DepL5009; ") + kLowDecls5009;
}

std::string appLow5009(const std::string& tag, const std::string& call) {
    return "module AppL5009" + tag +
           "; import DepL5009; class CL5009 public static method Main(): "
           "int using var s := new SqL5009(); return " + call +
           "; end Main; end CL5009;";
}

// #1943's reject, as the pre-attach refusal leaves it for `name`.
std::string widthReject5009(const std::string& name) {
    return "calling imported generic method `" + name +
           "` with a type argument whose representation is not the compiled "
           "`int` (i32) placeholder";
}

// The codegen backstop's reject for `name`.
std::string lowerReject5009(const std::string& name) {
    return "calling imported generic method `" + name +
           "`: its body could not be monomorphized across the module boundary";
}

}  // namespace

// ===========================================================================
// T1 — ★ a CLASS argument, T inferred from it (§14.5 phase 1). The body's
// `v.Area()` dispatches through the imported `IShape5009` bound. Pre-fix: the
// #1943 gate rejected the call (the pointer argument is not the i32
// placeholder), so the constraint-dispatch shape was unusable cross-module.
// // per §14.1 / §14.2 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           class_argument_dispatches_through_imported_bound) {
    int rc = buildImportAndRun5009(
        dep5009(),
        app5009("T1", "using var s := new Sq5009(); "
                      "return G5009.Gen5009(s);"));
    if (rc == -2) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T2 — ★ the same call with the type argument SPELLED OUT (§14.5 "explicit
// lists"), so the fix is not specific to the inference arm.
// // per §14.5 / §14.1 / §14.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           explicit_type_argument_dispatches_through_imported_bound) {
    int rc = buildImportAndRun5009(
        dep5009(),
        app5009("T2", "using var s := new Sq5009(); "
                      "return G5009.Gen5009<Sq5009>(s);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T3 — ★ a RECORD (value-kind) argument: the monomorphized body must take the
// record by value at its real layout and dispatch `Area()` to the record's
// own member. 3 * 4 = 12.
// // per §14.1 / §14.2 / §9.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           record_argument_dispatches_through_imported_bound) {
    int rc = buildImportAndRun5009(
        dep5009(),
        app5009("T3", "var r := new Rect5009(3, 4); "
                      "return G5009.Gen5009(r);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 12);
}

// ===========================================================================
// T4 — ★ a STRING-carrying record: the constraint member reads a string field
// (`Name.Length`), so a wrong layout or a dropped retain would show in the
// value (or under the gate's ASan leg). "hello".Length = 5.
// // per §14.1 / §14.2 / §9.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           string_carrying_record_argument_dispatches_through_imported_bound) {
    int rc = buildImportAndRun5009(
        dep5009(),
        app5009("T4", "var n := new Named5009(\"hello\"); "
                      "return G5009.Gen5009(n);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

// ===========================================================================
// T5 — ★ a STRING-carrying class. "abcd".Length + 1 = 5.
// // per §14.1 / §14.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           string_carrying_class_argument_dispatches_through_imported_bound) {
    int rc = buildImportAndRun5009(
        dep5009(),
        app5009("T5", "using var t := new Tag5009(\"abcd\"); "
                      "return G5009.Gen5009(t);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

// ===========================================================================
// T6 — ★ an INTERFACE-typed argument (T bound to `IShape5009` itself): the
// call dispatches through the interface's own slot at run time.
// // per §14.1 / §14.2 / §11.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           interface_typed_argument_dispatches_through_imported_bound) {
    int rc = buildImportAndRun5009(
        dep5009(),
        app5009("T6", "var s: IShape5009 := new Sq5009(); "
                      "return G5009.Gen5009(s);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T7 — ★ THE SILENT ZERO. `Mk5009<T>(): int where T: IShape5009, new()` has
// no type-parameter-typed argument, so #1943's width loop has nothing to
// compare and the pre-fix call ran the placeholder template, whose `new T()`
// (#2477) and `t.Area()` (#2491) both degraded: exit 0 with no diagnostic in
// the importing compile. The single-module program returns 7.
// // per §14.1 / §14.2 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           new_bound_without_typed_argument_returns_constraint_value) {
    int rc = buildImportAndRun5009(
        dep5009(), app5009("T7", "return G5009.Mk5009<Sq5009>();"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T8 — DIFFERENTIAL CONTROL. Every conformer through both methods, once
// across the boundary and once in ONE module: the two must agree, and both
// must equal the hand-computed 7 + 12 + 5 + 4 + 7 = 35 —
// so §2.8.3 rule 2 is checked directly, not just against a constant.
// // per §2.8.3 rule 2 / §14.1, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           cross_module_matches_same_module_twin) {
    BEZEL_EXPECT_EQ(buildLocalAndRun5009(local5009("T8L", kAllBody5009)), 35);
    int rc = buildImportAndRun5009(dep5009(), app5009("T8X", kAllBody5009));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 35);
}

// ===========================================================================
// T9 — NO-REGRESSION. A BUILTIN interface bound (`where T: INumber<T>`) is
// deliberately NOT shipped: the importer's re-parse context would resynthesize
// `INumber` as a different symbol and the §14.2 call-site check then rejects
// the conforming `int`. MEASURED while developing this fix: with the body
// shipped this row was rejected. The body does not use the bound, so the
// template path it keeps is correct: 4.
// // per §14.2 / stdlib §2.19, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           builtin_interface_bound_keeps_working_template_path) {
    int rc = buildImportAndRun5009(
        dep5009(), app5009("T9", "return G5009.GenNum5009<int>(5);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 4);
}

// ===========================================================================
// T10 — NO-REGRESSION. A special-only bound (`where T: value`) grants no
// member access, so nothing ships and #1943's argument-width gate must still
// reject a `double` instantiation with its SPECIFIC diagnostic.
// // per §14.1 / §2.8.3 / #1943, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           special_only_bound_still_argument_width_rejected) {
    auto r = compileImport5009(
        dep5009(),
        app5009("T10", "var n := G5009.Count5009<double>(3.0); return n;"));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(r.err.find(kArgWidthReject5009) != std::string::npos);
}

// ===========================================================================
// T11 — ★ REVIEW REGRESSION q1d. The body calls a PUBLIC sibling's static
// method, inline and through a local. 539563d5 attached the body but dropped
// the `HelperS5009` / `Bonus` bindings, and printed `0` and `7` for a program
// dcb17d57 rejected cleanly. 7 + 20 = 27 for both.
// // per §14.1 / §2.8.3 rule 2 / rule 4, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           sibling_static_call_in_body_returns_full_value) {
    int rc = buildImportAndRun5009(
        depSib5009(),
        appSib5009("T11a", "", "using var s := new SqS5009(); "
                               "return GS5009.Gen(s);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 27);
    rc = buildImportAndRun5009(
        depSib5009(),
        appSib5009("T11b", "", "using var s := new SqS5009(); "
                               "return GS5009.Gen2(s);"));
    BEZEL_EXPECT_EQ(rc, 27);
}

// ===========================================================================
// T12 — ★ REVIEW REGRESSION q1i. A sibling's instance field (`b.V`), instance
// method (`b.Twice()`) and static readonly field (`ConstsS5009.K`).
// 539563d5 printed `0 0 0`. 11 + 15 + 47 = 73. Every term is positive, so a
// dropped one cannot hide in the sum.
// // per §14.1 / §2.8.3 rule 2 / rule 4, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           sibling_field_method_and_static_field_in_body_return_full_value) {
    int rc = buildImportAndRun5009(
        depSib5009(),
        appSib5009("T12", "", "using var s := new SqS5009(); "
                              "return GS5009.GBox(s) + GS5009.GTwice(s) + "
                              "GS5009.GK(s);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 73);
}

// ===========================================================================
// T13 — ★ REVIEW REGRESSION q1f. The importer's OWN conformer (`MineS5009`,
// Area 3) through the sibling-calling body: 3 + 20 = 23. 539563d5: 3.
// // per §14.1 / §14.2 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           importer_own_conformer_through_sibling_calling_body) {
    int rc = buildImportAndRun5009(
        depSib5009(),
        appSib5009("T13",
                   "internal class MineS5009: IShapeS5009 "
                   "  public constructor() end constructor; "
                   "  public method Area(): int return 3; end Area; "
                   "end MineS5009; ",
                   "using var m := new MineS5009(); return GS5009.Gen2(m);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 23);
}

// ===========================================================================
// T14 — ★ `new()`-only bodies (no T-typed argument, so #1943 cannot catch a
// wrong value) calling a sibling's static method and the own class's static
// method. The importer declares its own `HelperS5009` (Bonus 50), which must
// NOT capture the dependency's. 27 + 37 = 64.
// // per §14.1 / §14.2 / §2.8.3 rule 4, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           new_bound_bodies_calling_statics_ignore_importer_homonym) {
    int rc = buildImportAndRun5009(
        depSib5009(),
        appSib5009("T14",
                   "internal static class HelperS5009 "
                   "  public static method Bonus(): int return 50; end Bonus; "
                   "end HelperS5009; ",
                   "return GS5009.Mk<SqS5009>() + GS5009.MkSelf<SqS5009>();"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 64);
}

// ===========================================================================
// T15 — ★ a self-QUALIFIED static call (`GS5009.PubHelp()` inside `GS5009`).
// The qualifier names the synthetic self type, which is rebound to the
// imported one. 7 + 30 = 37.
// // per §14.1 / §2.8.3 rule 4, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           self_qualified_static_call_in_body_returns_full_value) {
    int rc = buildImportAndRun5009(
        depSib5009(),
        appSib5009("T15", "", "using var s := new SqS5009(); "
                              "return GS5009.QSelf(s);"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 37);
}

// ===========================================================================
// T16 — ★ FAIL CLOSED. `GenPS5009` reads a COMPUTED property of its own
// class, and the importer's remap has no arm that binds it. The body must NOT
// be attached, so the call keeps #1943's clean reject. MEASURED with the
// fail-closed return disabled: the call compiled with an INTERNAL #2491
// warning and returned 0 instead of 12.
// // per §14.1 / §2.8.3 / #1943, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           unbindable_body_reference_fails_closed_to_width_reject) {
    auto r = compileImport5009(
        depSib5009(),
        appSib5009("T16", "", "using var s := new SqS5009(); "
                              "using var a := new AccS5009(); "
                              "return a.GenPS5009(s);"));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(r.err.find(kFailClosedReject5009) != std::string::npos);
}

// ===========================================================================
// T17 — DIFFERENTIAL CONTROL for T11-T15: the same bodies in ONE module and
// across the boundary must both answer 228.
// // per §2.8.3 rule 2 / §14.1, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           sibling_reaching_bodies_match_same_module_twin) {
    BEZEL_EXPECT_EQ(
        buildLocalAndRun5009(std::string("module LocS5009; ") +
                             kSibDecls5009 +
                             "class CS5009 public static method Main(): int " +
                             kSibAllBody5009 + " end Main; end CS5009;"),
        228);
    int rc = buildImportAndRun5009(depSib5009(),
                                   appSib5009("T17", "", kSibAllBody5009));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 228);
}

// ===========================================================================
// T18-T23 — ★ SECOND REVIEW REGRESSIONS (43854953). A body containing a lambda
// (p8b / q1 / q3 / q4) or taking a member of a nested generic call's result
// (p6c inferred / q7 explicit) bound cleanly but did not lower: each returned
// 0 with an INTERNAL #2491 warning. On the constraint route such a body is now
// refused before it is attached, so each call keeps #1943's clean reject,
// exactly as on dcb17d57. Asserting the reject, not the value: the value
// would need importer-side lambda and nested-generic monomorphization, which
// is not proven.
// // per §14.1 / §14.2 / §27.3 / §2.8.3 / #1943, v0.307
// ===========================================================================
namespace {
void expectWidthReject5009(const std::string& tag, const std::string& name) {
    auto r = compileImport5009(depLow5009(),
                               appLow5009(tag, "GL5009." + name + "(s)"));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(r.err.find(widthReject5009(name)) != std::string::npos);
}
}  // namespace

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           lambda_capturing_bound_value_and_own_static_is_rejected) {
    expectWidthReject5009("T18", "LamCapHelp");  // p8b
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           lambda_capturing_bound_value_is_rejected) {
    expectWidthReject5009("T19", "LamCap");  // q1
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           capture_free_lambda_calling_sibling_static_is_rejected) {
    expectWidthReject5009("T20", "LamSib");  // q3
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           lambda_with_type_parameter_parameter_is_rejected) {
    expectWidthReject5009("T21", "LamParam");  // q4
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           member_of_inferred_nested_generic_call_result_is_rejected) {
    expectWidthReject5009("T22", "IdInfer");  // p6c
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           member_of_explicit_nested_generic_call_result_is_rejected) {
    expectWidthReject5009("T23", "IdExplicit");  // q7
}

// ===========================================================================
// T24 — ★ THE CODEGEN BACKSTOP. `GenBox` instantiates a sibling generic class
// at T. It has no lambda and no generic method call, so the pre-attach refusal
// lets it through, and on the pre-backstop tree it returned 0 with an INTERNAL
// #2491 warning (MEASURED). Any INTERNAL degrade while a constraint-route body
// is emitted is now a located error at the call site.
// // per §14.1 / §2.8.3 rule 2 / #1943, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           body_that_does_not_lower_is_rejected_not_zero) {
    auto r = compileImport5009(depLow5009(),
                               appLow5009("T24", "GL5009.GenBox(s)"));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(r.err.find(lowerReject5009("GenBox")) !=
                      std::string::npos);
}

// ===========================================================================
// T25 — DIFFERENTIAL CONTROL for T18-T24: in ONE module every body lowers,
// so the rejects above are a module-boundary limitation, not a wrong program.
// 37 + 107 + 27 + 107 + 107 + 107 + 107 = 599; minus 500 to stay within the
// 8-bit exit status = 99.
// // per §2.8.3 rule 2 / §14.1 / §27.3, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           non_lowering_bodies_match_values_in_one_module) {
    BEZEL_EXPECT_EQ(
        buildLocalAndRun5009(
            std::string("module LocL5009; ") + kLowDecls5009 +
            "class CL5009 public static method Main(): int "
            "using var s := new SqL5009(); "
            "return GL5009.LamCapHelp(s) + GL5009.LamCap(s) + "
            "GL5009.LamSib(s) + GL5009.LamParam(s) + GL5009.IdInfer(s) + "
            "GL5009.IdExplicit(s) + GL5009.GenBox(s) - 500; "
            "end Main; end CL5009;"),
        99);
}

// ===========================================================================
// The fourth dependency, for T26-T31 (the third non-author review of
// bd0405e6). VIRTUAL generic methods whose own type parameter has a sibling
// bound. A virtual call is lowered by emitVirtualGenericDispatch, not
// emitInlineGenericMethod, so on bd0405e6 the shipped body bypassed both the
// pre-attach refusal and the backstop.
// ===========================================================================
namespace {
const char* kVirtDecls5009 =
    "public interface IShapeV5009 "
    "  method Area(): int; "
    "end IShapeV5009; "
    "public class SqV5009: IShapeV5009 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end SqV5009; "
    "public class BoxV5009<U> "
    "  private _u: U; "
    "  public constructor(u: U) _u := u; end constructor; "
    "  public method Get(): U return _u; end Get; "
    "end BoxV5009; "
    // (a): a sibling generic class at T. 7 + 100 = 107.
    "public class GVA5009 "
    "  public constructor() end constructor; "
    "  public virtual method VBox<T>(v: T): int where T: IShapeV5009 "
    "    var b := new BoxV5009<T>(v); "
    "    return b.Get().Area() + 100; "
    "  end VBox; "
    "end GVA5009; "
    // (b): the same, overridden. Through a GVB5009 receiver on a GVB2:
    // 7 + 200 = 207.
    "public class GVB5009 "
    "  public constructor() end constructor; "
    "  public virtual method VBox<T>(v: T): int where T: IShapeV5009 "
    "    var b := new BoxV5009<T>(v); "
    "    return b.Get().Area() + 100; "
    "  end VBox; "
    "end GVB5009; "
    "public class GVB2V5009: GVB5009 "
    "  public constructor() end constructor; "
    "  public override method VBox<T>(v: T): int where T: IShapeV5009 "
    "    return v.Area() + 200; "
    "  end VBox; "
    "end GVB2V5009; "
    // (c): a plain body that LOWERS, overridden. Through a GVC5009 receiver
    // on a GVC2: 7 + 200 = 207.
    "public class GVC5009 "
    "  public constructor() end constructor; "
    "  public virtual method VPlain<T>(v: T): int where T: IShapeV5009 "
    "    var k := 0; "
    "    return v.Area() + 100 + k; "
    "  end VPlain; "
    "end GVC5009; "
    "public class GVC2V5009: GVC5009 "
    "  public constructor() end constructor; "
    "  public override method VPlain<T>(v: T): int where T: IShapeV5009 "
    "    return v.Area() + 200; "
    "  end VPlain; "
    "end GVC2V5009; ";

const char* kVirtCallA5009 =
    "using var g := new GVA5009(); return g.VBox(s);";
const char* kVirtCallB5009 =
    "using var g := new GVB2V5009(); var b: GVB5009 := g; return b.VBox(s);";
const char* kVirtCallC5009 =
    "using var g := new GVC2V5009(); var b: GVC5009 := g; "
    "return b.VPlain(s);";

std::string mainV5009(const std::string& call) {
    return "class CV5009 public static method Main(): int "
           "using var s := new SqV5009(); " + call +
           " end Main; end CV5009;";
}

void expectVirtualReject5009(const std::string& tag, const std::string& call,
                             const std::string& name) {
    auto r = compileImport5009(
        std::string("module DepV5009; ") + kVirtDecls5009,
        "module AppV5009" + tag + "; import DepV5009; " + mainV5009(call));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(r.err.find(widthReject5009(name)) != std::string::npos);
}

int runVirtualLocal5009(const std::string& tag, const std::string& call) {
    return buildLocalAndRun5009(std::string("module LocV5009") + tag + "; " +
                                kVirtDecls5009 + mainV5009(call));
}
}  // namespace

// ===========================================================================
// T26 — ★ THIRD REVIEW, shape (a). A virtual generic method whose body
// instantiates a sibling generic class at T. On bd0405e6 its body was shipped
// and lowered by emitVirtualGenericDispatch, outside both of fix-forward 2's
// layers: exit 0, twin 107 (MEASURED). The body is no longer shipped, and the
// vtable-slot call gets #1943's located diagnostic (b30c6f12: an unlocated
// IR-verifier abort).
// // per §14.1 / §2.8.3 rule 4 / #1943, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           virtual_generic_method_body_is_rejected_not_zero) {
    expectVirtualReject5009("T26", kVirtCallA5009, "VBox");
}

// ===========================================================================
// T27 — ★ shape (b): the same body, overridden, called through a base-typed
// receiver on the derived object. bd0405e6: 0, twin 207 (MEASURED).
// // per §14.1 / §12 / §2.8.3 rule 4 / #1943, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           overridden_virtual_generic_method_is_rejected_not_zero) {
    expectVirtualReject5009("T27", kVirtCallB5009, "VBox");
}

// ===========================================================================
// T28 — ★ shape (c), SILENT WRONG CODE on bd0405e6: a plain body that lowers,
// overridden, through a base-typed receiver on the derived object. The
// cross-module dispatch ran the BASE body: 107 with no warning, twin 207
// (MEASURED). The override-selection hole itself is #5031; here the call is
// kept off the constraint route, so it is rejected.
// // per §14.1 / §12 / §2.8.3 rule 4 / #1943 / #5031, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           overridden_virtual_plain_body_is_rejected_not_base_value) {
    expectVirtualReject5009("T28", kVirtCallC5009, "VPlain");
}

// ===========================================================================
// T29-T31 — DIFFERENTIAL CONTROLS for T26-T28: in ONE module each virtual
// call dispatches to the right body with its value.
// // per §14.1 / §12 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           virtual_generic_method_body_value_in_one_module) {
    BEZEL_EXPECT_EQ(runVirtualLocal5009("T29", kVirtCallA5009), 107);
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           overridden_virtual_generic_method_value_in_one_module) {
    BEZEL_EXPECT_EQ(runVirtualLocal5009("T30", kVirtCallB5009), 207);
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           overridden_virtual_plain_body_value_in_one_module) {
    BEZEL_EXPECT_EQ(runVirtualLocal5009("T31", kVirtCallC5009), 207);
}

// ===========================================================================
// T32-T42 — FOURTH REVIEW (of fix-forward 3, 8575510a). A virtual generic
// method whose type argument crosses in NO parameter slips past the #1943
// width gate on the vtable-slot call: every argument has the placeholder's
// width, the body is not shipped (fix-forward 3), so the slot runs the
// defining module's i32-placeholder template. MEASURED on 8575510a, with no
// diagnostic and no internal warning: VNew 0 (twin 95), VCnt 0 (twin 47),
// VCntSub 0 (twin 48), VCntSubD 0 (twin 48). The non-virtual NNew keeps the
// constraint route and 95. Fix-forward 4 refuses the call on its BOUND (a
// `new()` bound or one declared by the exporting module), not its widths.
// ===========================================================================
namespace {
const char* kBoundDecls5009 =
    "public interface IShapeB5009 "
    "  method Area(): int; "
    "end IShapeB5009; "
    "public class SqB5009: IShapeB5009 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end SqB5009; "
    // VNew: 7 + 88 = 95.
    "public class GBNew5009 "
    "  public constructor() end constructor; "
    "  public virtual method Mk<T>(): int where T: IShapeB5009, new() "
    "    var t := new T(); "
    "    return t.Area() + 88; "
    "  end Mk; "
    "end GBNew5009; "
    // VCnt: 7 + 40 = 47; the override 7 + 40 + 1 = 48.
    "public class GBCnt5009 "
    "  public constructor() end constructor; "
    "  public virtual method Cnt<T>(n: int): int where T: IShapeB5009, new() "
    "    var t := new T(); "
    "    return t.Area() + n; "
    "  end Cnt; "
    "end GBCnt5009; "
    "public class GBCnt2V5009: GBCnt5009 "
    "  public constructor() end constructor; "
    "  public override method Cnt<T>(n: int): int where T: IShapeB5009, new() "
    "    var t := new T(); "
    "    return t.Area() + n + 1; "
    "  end Cnt; "
    "end GBCnt2V5009; "
    // NNew (non-virtual, stays on the constraint route): 7 + 88 = 95.
    "public class GBNNew5009 "
    "  public constructor() end constructor; "
    "  public method Mk<T>(): int where T: IShapeB5009, new() "
    "    var t := new T(); "
    "    return t.Area() + 88; "
    "  end Mk; "
    "end GBNNew5009; "
    // A BUILTIN bound only: the i32 template is right, so it is not refused.
    // 40 + 5 = 45.
    "public class GBCls5009 "
    "  public constructor() end constructor; "
    "  public virtual method Tag<T>(n: int): int where T: class "
    "    return n + 5; "
    "  end Tag; "
    "end GBCls5009; ";

const char* kBoundNew5009 =
    "using var g := new GBNew5009(); return g.Mk<SqB5009>();";
const char* kBoundCnt5009 =
    "using var g := new GBCnt5009(); return g.Cnt<SqB5009>(40);";
const char* kBoundCntSub5009 =
    "using var g := new GBCnt2V5009(); var b: GBCnt5009 := g; "
    "return b.Cnt<SqB5009>(40);";
const char* kBoundCntSubD5009 =
    "using var g := new GBCnt2V5009(); return g.Cnt<SqB5009>(40);";
const char* kBoundNNew5009 =
    "using var g := new GBNNew5009(); return g.Mk<SqB5009>();";
const char* kBoundCls5009 =
    "using var g := new GBCls5009(); return g.Tag<SqB5009>(40);";

std::string mainB5009(const std::string& call) {
    return "class CB5009 public static method Main(): int " + call +
           " end Main; end CB5009;";
}

void expectBoundReject5009(const std::string& tag, const std::string& call,
                           const std::string& name) {
    auto r = compileImport5009(
        std::string("module DepB5009; ") + kBoundDecls5009,
        "module AppB5009" + tag + "; import DepB5009; " + mainB5009(call));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(r.err.find(lowerReject5009(name)) != std::string::npos);
}

int runBoundImport5009(const std::string& tag, const std::string& call) {
    return buildImportAndRun5009(
        std::string("module DepB5009") + tag + "; " + kBoundDecls5009,
        "module AppB5009" + tag + "; import DepB5009" + tag + "; " +
            mainB5009(call));
}

int runBoundLocal5009(const std::string& tag, const std::string& call) {
    return buildLocalAndRun5009(std::string("module LocB5009") + tag + "; " +
                                kBoundDecls5009 + mainB5009(call));
}
}  // namespace

// T32-T35 — ★ the silent zeros, now located rejects.
// // per §14.1 / §14.2 new() / §2.8.3 rule 4 / #1943, v0.307
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           virtual_new_bound_method_without_t_argument_is_rejected_not_zero) {
    expectBoundReject5009("T32", kBoundNew5009, "Mk");
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           virtual_bound_method_with_only_int_argument_is_rejected_not_zero) {
    expectBoundReject5009("T33", kBoundCnt5009, "Cnt");
}

// // per §14.1 / §12 / §2.8.3 rule 4 / #1943 / #5031, v0.307
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           overridden_bound_method_via_base_receiver_is_rejected_not_zero) {
    expectBoundReject5009("T34", kBoundCntSub5009, "Cnt");
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           overridden_bound_method_via_derived_receiver_is_rejected_not_zero) {
    expectBoundReject5009("T35", kBoundCntSubD5009, "Cnt");
}

// T36 — the NON-virtual twin keeps the constraint route and its value.
// // per §14.1 / §14.2 new() / §2.8.3 rule 4, v0.307
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           non_virtual_new_bound_method_keeps_value_across_modules) {
    int rc = runBoundImport5009("T36", kBoundNNew5009);
    if (rc == -2) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 95);
}

// T37 — a BUILTIN-only bound is not refused: the i32 template is right.
// // per §14.1 / §14.2 / §2.8.3 rule 4, v0.307
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           virtual_builtin_bound_method_keeps_value_across_modules) {
    int rc = runBoundImport5009("T37", kBoundCls5009);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 45);
}

// T38-T42 — DIFFERENTIAL CONTROLS: in ONE module every call has its value.
// // per §14.1 / §12 / §2.8.3 rule 2, v0.307
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           virtual_new_bound_method_value_in_one_module) {
    BEZEL_EXPECT_EQ(runBoundLocal5009("T38", kBoundNew5009), 95);
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           virtual_bound_method_int_argument_value_in_one_module) {
    BEZEL_EXPECT_EQ(runBoundLocal5009("T39", kBoundCnt5009), 47);
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           overridden_bound_method_base_receiver_value_in_one_module) {
    BEZEL_EXPECT_EQ(runBoundLocal5009("T40", kBoundCntSub5009), 48);
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           overridden_bound_method_derived_receiver_value_in_one_module) {
    BEZEL_EXPECT_EQ(runBoundLocal5009("T41", kBoundCntSubD5009), 48);
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           non_virtual_new_bound_method_value_in_one_module) {
    BEZEL_EXPECT_EQ(runBoundLocal5009("T42", kBoundNNew5009), 95);
}

// ===========================================================================
// T43-T50 — FIFTH REVIEW (of fix-forward 4, f7554738): a bound declared in a
// THIRD module. Dep1 declares the interface, a class implementing it and a
// generic box; Dep2 imports Dep1 and declares generic methods bounded by
// Dep1's interface, taking `RBox<T>`; the app imports both. The exporter
// ships a body only for a bound its own module declares, so Dep2 ships none,
// and `RBox<T>` crosses as a pointer, so the #1943 width gate never fires.
// MEASURED on b30c6f12, 8575510a and f7554738: the virtual VB3 AND the
// non-virtual NB3 returned 0 with no diagnostic (single-module twin 127).
// Fix-forward 5 counts a user-type bound from any module, on the slot call
// and on the direct call.
// ===========================================================================
namespace {
const char* kThirdDep1_5009 =
    "public interface IShape3M5009 "
    "  method Area(): int; "
    "end IShape3M5009; "
    "public class Sq3M5009: IShape3M5009 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end Sq3M5009; "
    "public class Box3M5009<U> "
    "  private _u: U; "
    "  public constructor(u: U) _u := u; end constructor; "
    "  public method Get(): U return _u; end Get; "
    "end Box3M5009; ";
const char* kThirdDep2_5009 =
    // 7 + 120 = 127 for both.
    "public class G3M5009 "
    "  public constructor() end constructor; "
    "  public virtual method VB3<T>(b: Box3M5009<T>): int "
    "      where T: IShape3M5009 "
    "    return b.Get().Area() + 120; "
    "  end VB3; "
    "  public method NB3<T>(b: Box3M5009<T>): int where T: IShape3M5009 "
    "    return b.Get().Area() + 120; "
    "  end NB3; "
    // BUILTIN bounds only: the i32 template is right, so neither is
    // refused. 45 and 95.
    "  public method NCls3<T>(b: Box3M5009<T>): int where T: class "
    "    return 45; "
    "  end NCls3; "
    "  public method NOnly3<T>(): int where T: new() "
    "    var t := new T(); "
    "    return 95; "
    "  end NOnly3; "
    "end G3M5009; ";

const char* kThirdVB3_5009 =
    "using var g := new G3M5009(); "
    "var bx := new Box3M5009<Sq3M5009>(new Sq3M5009()); return g.VB3(bx);";
const char* kThirdNB3_5009 =
    "using var g := new G3M5009(); "
    "var bx := new Box3M5009<Sq3M5009>(new Sq3M5009()); return g.NB3(bx);";
const char* kThirdNCls3_5009 =
    "using var g := new G3M5009(); "
    "var bx := new Box3M5009<Sq3M5009>(new Sq3M5009()); return g.NCls3(bx);";
const char* kThirdNOnly3_5009 =
    "using var g := new G3M5009(); return g.NOnly3<Sq3M5009>();";

std::string mainThird5009(const std::string& call) {
    return "class C3M5009 public static method Main(): int " + call +
           " end Main; end C3M5009;";
}

// Build Dep1, then Dep2 against it, then compile the app against both; if
// `run`, link and run it too. `exitCode` is -1 on a compile/link/run failure.
struct Third5009 {
    bool depOk = true;
    bool success = false;
    int exitCode = -1;
    std::string err;
};
Third5009 buildThird5009(const std::string& tag, const std::string& call,
                         bool run) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5009t_");
    TempFile5009 s1((dir / (stem + "_d1.bzl")).string());
    TempFile5009 o1((dir / (stem + "_d1.o")).string());
    TempFile5009 s2((dir / (stem + "_d2.bzl")).string());
    TempFile5009 o2((dir / (stem + "_d2.o")).string());
    TempFile5009 sa((dir / (stem + "_a.bzl")).string());
    TempFile5009 exe((dir / stem).string());
    const std::string m1 = "Dep3A5009" + tag, m2 = "Dep3B5009" + tag;
    writeSource5009(s1.path, "module " + m1 + "; " + kThirdDep1_5009);
    writeSource5009(s2.path, "module " + m2 + "; import " + m1 + "; " +
                                 kThirdDep2_5009);
    writeSource5009(sa.path, "module App3M5009" + tag + "; import " + m1 +
                                 "; import " + m2 + "; " +
                                 mainThird5009(call));
    auto dep = [](const std::string& obj) {
        bezel::Dependency d;
        d.metadataPath = obj;
        d.objectPath = obj;
        return d;
    };
    Third5009 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(s1.path, o1.path).success ||
            !std::ifstream(o1.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    {
        bezel::Driver driver;
        if (!driver.compileObject(s2.path, o2.path, {dep(o1.path)}).success ||
            !std::ifstream(o2.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    r.err = captureStderr5009([&] {
        bezel::Driver driver;
        r.success = bezel::test::compileFileForTest(
                        driver, sa.path, exe.path,
                        {dep(o1.path), dep(o2.path)})
                        .success;
    });
    if (r.success && run) {
        int status = bezel::test::runProgramForTest(exe.path);
        r.exitCode =
            (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    }
    return r;
}

void expectThirdReject5009(const std::string& tag, const std::string& call,
                           const std::string& name) {
    Third5009 r = buildThird5009(tag, call, /*run=*/false);
    if (!r.depOk) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(r.err.find(lowerReject5009(name)) != std::string::npos);
}

int runThirdImport5009(const std::string& tag, const std::string& call) {
    Third5009 r = buildThird5009(tag, call, /*run=*/true);
    if (!r.depOk) return -2;
    return r.exitCode;
}

int runThirdLocal5009(const std::string& tag, const std::string& call) {
    return buildLocalAndRun5009(std::string("module Loc3M5009") + tag + "; " +
                                kThirdDep1_5009 + kThirdDep2_5009 +
                                mainThird5009(call));
}
}  // namespace

// T43 / T44 — ★ the silent zeros, now located rejects: the slot call and
// the direct call.
// // per §14.1 / §14.2 / §2.8.3 rule 4 / #1943, v0.307
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           third_module_bound_virtual_method_is_rejected_not_zero) {
    expectThirdReject5009("T43", kThirdVB3_5009, "VB3");
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           third_module_bound_non_virtual_method_is_rejected_not_zero) {
    expectThirdReject5009("T44", kThirdNB3_5009, "NB3");
}

// T45 / T46 — builtin bounds on the direct call keep their values across
// three modules.
// // per §14.1 / §14.2 / §2.8.3 rule 4, v0.307
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           third_module_class_bound_direct_call_keeps_value) {
    int rc = runThirdImport5009("T45", kThirdNCls3_5009);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 45);
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           third_module_new_only_bound_direct_call_keeps_value) {
    int rc = runThirdImport5009("T46", kThirdNOnly3_5009);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 95);
}

// T47-T50 — DIFFERENTIAL CONTROLS: in ONE module every call has its value.
// // per §14.1 / §12 / §2.8.3 rule 2, v0.307
BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           third_module_bound_virtual_method_value_in_one_module) {
    BEZEL_EXPECT_EQ(runThirdLocal5009("T47", kThirdVB3_5009), 127);
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           third_module_bound_non_virtual_method_value_in_one_module) {
    BEZEL_EXPECT_EQ(runThirdLocal5009("T48", kThirdNB3_5009), 127);
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           third_module_class_bound_value_in_one_module) {
    BEZEL_EXPECT_EQ(runThirdLocal5009("T49", kThirdNCls3_5009), 45);
}

BEZEL_TEST(xmod_constraint_dispatch_generic_method_5009,
           third_module_new_only_bound_value_in_one_module) {
    BEZEL_EXPECT_EQ(runThirdLocal5009("T50", kThirdNOnly3_5009), 95);
}

}  // namespace bezel::module
