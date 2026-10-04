// Regression tests for #5214: a library's INTERNAL call of its own virtual
// generic method ran the library's own body for an instance of a class the
// library cannot see (declared in a module compiled after it), whatever that
// class's most-derived implementation was — a silent wrong value.
//
// Spec: §11 / §12.1 (a call through a base-typed receiver runs the receiver's
// most-derived override, including one declared in an importing module),
// §14.1 / §14.2 (a generic method is monomorphized per type argument), §2.8.3
// rule 4 (an imported generic is instantiated in the importing module),
// v0.313.
//
// The library's call site is compiled before the importer exists, so it can
// specialize only the implementations it sees. The fix (narrow #5214) reads
// the receiver's vtable slot for the method on the default path of a LIBRARY
// compile:
//   * descriptor of a known class      → that class runs the default;
//   * slot = the default's function    → the class inherits the default; run
//                                         it (rows A, B, C, E, ESub1: 127);
//   * slot = a known override's fn     → run that override (PlainD: 207,
//                                         ESub2 through SB: 307 — both were a
//                                         silent 127);
//   * any other slot                   → an override the library cannot
//                                         specialize: abort with a message
//                                         naming #5214 (the literal repro,
//                                         ESub2 through SA, Ebase, EGO — all
//                                         were a silent 127).
// The literal repro's correct value is 37 (the single-module twin pins it);
// computing 37 across the boundary needs the importer to specialize its
// override at the library's type arguments, which the narrow does not do.
//
// Also pinned: the vtable-slot key of an IMPORTED method whose parameter is a
// user type (`Take(s: Sq)`) now spells the type's name, so an importer's
// override lands in the base's slot (12, was a silent 8) — the same key
// mismatch lost an imported override of an imported virtual generic method in
// a third module (ESub2 through SB).
//
// Every value row asserts the program's STDOUT (`v=<n>`), never only the exit
// code (307 would truncate to 51); every trap row asserts SIGABRT and the
// message on stderr.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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

struct TempFile5214 {
    std::string path;
    explicit TempFile5214(std::string p) : path(std::move(p)) {}
    ~TempFile5214() { std::remove(path.c_str()); }
};

void writeSource5214(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Run `fn` with file descriptor `fd` (1 or 2) redirected into a file; return
// what was written. A child process started inside `fn` inherits the
// redirection, which is how the program's own output is captured.
template <typename Fn>
std::string captureFd5214(int fd, Fn&& fn) {
    std::string path =
        (fs::temp_directory_path() /
         ("bz5214_cap_" + unique_stem("") + ".txt")).string();
    std::fflush(stdout);
    std::fflush(stderr);
    std::cout.flush();
    std::cerr.flush();
    int saved = ::dup(fd);
    int f = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (f >= 0) ::dup2(f, fd);
    fn();
    std::fflush(stdout);
    std::fflush(stderr);
    std::cout.flush();
    std::cerr.flush();
    if (f >= 0) {
        ::close(f);
        ::dup2(saved, fd);
    }
    ::close(saved);
    std::ifstream in(path);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(path.c_str());
    return out;
}

struct Run5214 {
    bool built = false;  // every object and the executable compiled
    int status = -1;     // raw wait status of the program
    std::string out;     // the program's stdout
    std::string err;     // the program's stderr
};

// Compile every source but the last as a library object (each importing all
// earlier ones), the last as an executable importing them all, and run it.
Run5214 buildChainAndRun5214(const std::vector<std::string>& srcs) {
    Run5214 r;
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5214_");
    std::vector<std::unique_ptr<TempFile5214>> keep;
    std::vector<bezel::Dependency> deps;
    for (std::size_t i = 0; i + 1 < srcs.size(); ++i) {
        auto src = std::make_unique<TempFile5214>(
            (dir / (id + "_m" + std::to_string(i) + ".bzl")).string());
        auto obj = std::make_unique<TempFile5214>(
            (dir / (id + "_m" + std::to_string(i) + ".o")).string());
        writeSource5214(src->path, srcs[i]);
        bezel::Driver driver;
        if (!driver.compileObject(src->path, obj->path, deps).success ||
            !std::ifstream(obj->path).good())
            return r;
        bezel::Dependency d;
        d.metadataPath = obj->path;
        d.objectPath = obj->path;
        deps.push_back(d);
        keep.push_back(std::move(src));
        keep.push_back(std::move(obj));
    }
    TempFile5214 mainSrc((dir / (id + "_main.bzl")).string());
    TempFile5214 mainExe((dir / id).string());
    writeSource5214(mainSrc.path, srcs.back());
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, mainSrc.path, mainExe.path,
                                         deps)
             .success)
        return r;
    r.built = true;
    r.err = captureFd5214(2, [&] {
        r.out = captureFd5214(1, [&] {
            r.status = bezel::test::runProgramForTest(mainExe.path);
        });
    });
    return r;
}

// SIGABRT from bz_debug_assert_fail; under the sanitizer sweep the shell
// reports it as exit 128+6 (see runProgramForTest).
bool aborted5214(int st) {
    return st != -1 && ((WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT) ||
                        (WIFEXITED(st) && WEXITSTATUS(st) == 134));
}

// The program printed exactly `v=<expected>` and exited 0.
void expectValue5214(const Run5214& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_TRUE(r.status != -1 && WIFEXITED(r.status));
    BEZEL_EXPECT_EQ((r.status != -1 && WIFEXITED(r.status))
                        ? WEXITSTATUS(r.status) : -1, 0);
    BEZEL_EXPECT_EQ(r.out, "v=" + expected + "\n");
}

// The program aborted with the #5214 message before printing anything.
void expectTrap5214(const Run5214& r) {
    BEZEL_EXPECT_TRUE(r.built);
    BEZEL_EXPECT_TRUE(aborted5214(r.status));
    BEZEL_EXPECT_EQ(r.out, "");
    BEZEL_EXPECT_TRUE(r.err.find("#5214") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.err.find("compiled after it") != std::string::npos);
}

// The issue's library `da.bzl`, plus the library's own subclass without an
// override (`LSub5214`) and with one (`LDrv5214`), a two-type-argument
// virtual generic method, and a non-generic virtual method with a user-typed
// parameter (`Take`).
const char* kTypes5214 =
    "public interface IV5214 "
    "  method Area(): int; "
    "end IV5214; "
    "public class Sq5214: IV5214 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end Sq5214; "
    "public class Tri5214: IV5214 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 3; end Area; "
    "end Tri5214; "
    "public class Box5214<U> "
    "  private _u: U; "
    "  public constructor(u: U) _u := u; end constructor; "
    "  public method Get(): U return _u; end Get; "
    "end Box5214; "
    "public class GA5214 "
    "  public constructor() end constructor; "
    "  public virtual method VB<T>(b: Box5214<T>): int where T: IV5214 "
    "    return b.Get().Area() + 120; "
    "  end VB; "
    "  public virtual method V2<T, W>(b: Box5214<T>, c: Box5214<W>): int "
    "      where T: IV5214 where W: IV5214 "
    "    return b.Get().Area() * 10 + c.Get().Area() + 50; "
    "  end V2; "
    "  public virtual method Take(s: Sq5214): int "
    "    return s.Area() + 1; "
    "  end Take; "
    "  public static method Run(g: GA5214): int "
    "    using var s := new Sq5214(); "
    "    var bx := new Box5214<Sq5214>(s); "
    "    return g.VB(bx); "
    "  end Run; "
    "  public static method Run2(g: GA5214): int "
    "    using var s := new Sq5214(); "
    "    using var t := new Tri5214(); "
    "    var bs := new Box5214<Sq5214>(s); "
    "    var bt := new Box5214<Tri5214>(t); "
    "    return g.V2(bs, bt); "
    "  end Run2; "
    "  public static method RunT(g: GA5214): int "
    "    using var s := new Sq5214(); "
    "    return g.Take(s); "
    "  end RunT; "
    "end GA5214; "
    "public class LSub5214: GA5214 "
    "  public constructor() end constructor; "
    "end LSub5214; "
    "public class LDrv5214: GA5214 "
    "  public constructor() end constructor; "
    "  public override method VB<T>(b: Box5214<T>): int where T: IV5214 "
    "    return b.Get().Area() + 200; "
    "  end VB; "
    "end LDrv5214; ";

// A SECOND library importing the first: a subclass without an override, one
// with an override, and its own internal call site (`BR5214.RunB`).
const char* kLibB5214 =
    "public class BSub5214: GA5214 "
    "  public constructor() end constructor; "
    "end BSub5214; "
    "public class BDrv5214: GA5214 "
    "  public constructor() end constructor; "
    "  public override method VB<T>(b: Box5214<T>): int where T: IV5214 "
    "    return b.Get().Area() + 300; "
    "  end VB; "
    "end BDrv5214; "
    "public static class BR5214 "
    "  public static method RunB(g: GA5214): int "
    "    using var s := new Sq5214(); "
    "    var bx := new Box5214<Sq5214>(s); "
    "    return g.VB(bx); "
    "  end RunB; "
    "end BR5214; ";

// The issue's app `dm.bzl` (`Mine5214`), plus importer classes WITHOUT an
// override: of the library's base, of the library's overriding subclass, and
// a generic one; and importer overrides of other shapes.
const char* kApp5214 =
    "class Mine5214: GA5214 "
    "  public constructor() end constructor; "
    "  public override method VB<T>(b: Box5214<T>): int where T: IV5214 "
    "    return b.Get().Area() + 30; "
    "  end VB; "
    "  public override method V2<T, W>(b: Box5214<T>, c: Box5214<W>): int "
    "      where T: IV5214 where W: IV5214 "
    "    return b.Get().Area() * 10 + c.Get().Area() + 1; "
    "  end V2; "
    "end Mine5214; "
    "class Plain5214: GA5214 "
    "  public constructor() end constructor; "
    "end Plain5214; "
    "class PlainD5214: LDrv5214 "
    "  public constructor() end constructor; "
    "end PlainD5214; "
    "class GenPlain5214<X>: GA5214 "
    "  public constructor() end constructor; "
    "end GenPlain5214; "
    "class Ebase5214: GA5214 "
    "  public constructor() end constructor; "
    "  public override method VB<T>(b: Box5214<T>): int where T: IV5214 "
    "    return base.VB(b) + 1; "
    "  end VB; "
    "end Ebase5214; "
    "class EOv5214: GA5214 "
    "  public constructor() end constructor; "
    "  public override method Take(s: Sq5214): int "
    "    return s.Area() + 5; "
    "  end Take; "
    "end EOv5214; ";

// A GENERIC importer subclass WITH an override, kept out of kApp5214: its
// presence makes the single-module twin's dispatch bail to the erased
// template (0 for 37) — a pre-existing main-module defect recorded in the
// handback, not this fix's.
const char* kAppEGO5214 =
    "class EGO5214<X>: GA5214 "
    "  public constructor() end constructor; "
    "  public override method VB<T>(b: Box5214<T>): int where T: IV5214 "
    "    return b.Get().Area() + 40; "
    "  end VB; "
    "end EGO5214; "
;

// Importer classes over the second library's classes.
const char* kAppB5214 =
    "class ESub15214: BSub5214 "
    "  public constructor() end constructor; "
    "end ESub15214; "
    "class ESub25214: BDrv5214 "
    "  public constructor() end constructor; "
    "end ESub25214; "
    "class EG5214<X>: BSub5214 "
    "  public constructor() end constructor; "
    "end EG5214; ";

std::string mainOf5214(const std::string& expr) {
    return "class Main5214 public static method Main(): int "
           "Console.WriteLine($\"v={" + expr + "}\"); return 0; "
           "end Main; end Main5214;";
}

// Library SA, executable SM: `expr` computed in Main over the app classes.
Run5214 libInternal5214(const std::string& tag, const std::string& expr) {
    return buildChainAndRun5214(
        {"module SA5214" + tag + "; " + std::string(kTypes5214),
         "module SM5214" + tag + "; import SA5214" + tag + "; " +
             std::string(kApp5214) + mainOf5214(expr)});
}

// Library SA, library SB importing it, executable SM importing both.
Run5214 twoLibs5214(const std::string& tag, const std::string& expr) {
    return buildChainAndRun5214(
        {"module SA5214" + tag + "; " + std::string(kTypes5214),
         "module SB5214" + tag + "; import SA5214" + tag + "; " +
             std::string(kLibB5214),
         "module SM5214" + tag + "; import SA5214" + tag + "; import SB5214" +
             tag + "; " + std::string(kApp5214) + std::string(kAppB5214) +
             mainOf5214(expr)});
}

}  // namespace

// The language semantics the literal repro asks for: in ONE module the call
// dispatches to the override (37). Passes before and after the fix.
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_single_module_twin_runs_override_5214) {
    Run5214 r = buildChainAndRun5214(
        {"module One5214; " + std::string(kTypes5214) + std::string(kApp5214) +
         mainOf5214("GA5214.Run(new Mine5214())")});
    expectValue5214(r, "37");
}

// #5214 literal repro: `GA.Run(new Mine())` across the boundary. Pre-fix the
// library silently printed its own body's 127. With the narrow it aborts:
// the library cannot specialize Mine's override at `Sq` (37 is the value a
// full fix computes).
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_library_internal_call_importer_override_traps_5214) {
    expectTrap5214(libInternal5214("I", "GA5214.Run(new Mine5214())"));
}

// Probe A: the library's own subclass without an override runs the base body.
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_library_own_subclass_no_override_5214) {
    expectValue5214(libInternal5214("A", "GA5214.Run(new LSub5214())"), "127");
}

// Probe B: a base-class instance runs the base body.
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_library_base_instance_5214) {
    expectValue5214(libInternal5214("B", "GA5214.Run(new GA5214())"), "127");
}

// Probe C: an importer subclass WITHOUT an override inherits the library's
// body, which is the correct one — it must never abort (the rejected trap
// of 6112c230 returned 134 here).
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_importer_subclass_no_override_runs_base_5214) {
    expectValue5214(libInternal5214("C", "GA5214.Run(new Plain5214())"),
                    "127");
}

// An importer subclass of the library's OVERRIDING subclass, without an
// override of its own, runs that known override (207). Pre-fix: a silent 127.
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_importer_subclass_inherits_library_override_5214) {
    expectValue5214(libInternal5214("D", "GA5214.Run(new PlainD5214())"),
                    "207");
}

// A GENERIC importer subclass without an override (its per-instantiation
// vtable must keep the base's shared slot function) runs the base body.
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_14_1_importer_generic_subclass_no_override_5214) {
    expectValue5214(
        libInternal5214("E", "GA5214.Run(new GenPlain5214<string>())"),
        "127");
}

// An importer override that chains to `base.VB(b)` (§12.1): the library
// cannot run it, so it aborts. Pre-fix: a silent 127 where 128 is correct.
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_12_1_importer_base_chaining_override_traps_5214) {
    expectTrap5214(libInternal5214("F", "GA5214.Run(new Ebase5214())"));
}

// A GENERIC importer subclass WITH an override: its slot holds a
// per-instantiation copy the library cannot know, so it aborts. Pre-fix: a
// silent 127 where 47 is correct.
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_14_1_importer_generic_subclass_override_traps_5214) {
    expectTrap5214(buildChainAndRun5214(
        {"module SA5214G; " + std::string(kTypes5214),
         "module SM5214G; import SA5214G; " + std::string(kAppEGO5214) +
             mainOf5214("GA5214.Run(new EGO5214<string>())")}));
}

// Two type arguments: an importer subclass without an override runs the base
// body (7 * 10 + 3 + 50 = 123); one with an override aborts (pre-fix: a
// silent 123 where 74 is correct).
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_14_2_two_type_args_no_override_5214) {
    expectValue5214(libInternal5214("T1", "GA5214.Run2(new Plain5214())"),
                    "123");
}
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_14_2_two_type_args_importer_override_traps_5214) {
    expectTrap5214(libInternal5214("T2", "GA5214.Run2(new Mine5214())"));
}

// §12.1 slot key of an imported method with a USER-typed parameter: an
// importer's `override Take(s: Sq)` must land in the base's slot, so the
// library's non-generic virtual call runs it (12). Pre-fix: the override took
// a fresh slot and the library ran its own body — a silent 8.
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_12_1_importer_override_user_typed_param_slot_5214) {
    expectValue5214(libInternal5214("K", "GA5214.RunT(new EOv5214())"), "12");
}

// Two libraries. ESub1 (importer subclass of SB's non-overriding subclass)
// runs the base body from either library's call site.
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_two_libs_inherits_base_via_first_library_5214) {
    expectValue5214(twoLibs5214("E1A", "GA5214.Run(new ESub15214())"), "127");
}
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_two_libs_inherits_base_via_second_library_5214) {
    expectValue5214(twoLibs5214("E1B", "BR5214.RunB(new ESub15214())"),
                    "127");
}

// ESub2 (importer subclass of SB's OVERRIDING subclass, no override of its
// own): SB's call site knows BDrv's override and runs it — 307, which an exit
// code could not carry (51). Pre-fix: the imported override's slot key read
// `?` for its `Box<T>` parameter, it took a fresh slot in SM, and SB's call
// site ran the base body — a silent 127. SA's call site cannot know BDrv's
// override, so it aborts (pre-fix: a silent 127).
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_two_libs_inherits_second_library_override_via_second_5214) {
    expectValue5214(twoLibs5214("E2B", "BR5214.RunB(new ESub25214())"),
                    "307");
}
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_two_libs_inherits_second_library_override_via_first_traps_5214) {
    expectTrap5214(twoLibs5214("E2A", "GA5214.Run(new ESub25214())"));
}

// SB's own classes through each library's call site: BDrv runs its override
// from SB (307) and aborts from SA (pre-fix: a silent 127); LDrv (SA's
// override, known to both) runs it from SB (207); a generic importer subclass
// of BSub runs the base body (127).
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_two_libs_second_library_override_via_second_5214) {
    expectValue5214(twoLibs5214("BDB", "BR5214.RunB(new BDrv5214())"), "307");
}
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_two_libs_second_library_override_via_first_traps_5214) {
    expectTrap5214(twoLibs5214("BDA", "GA5214.Run(new BDrv5214())"));
}
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_two_libs_first_library_override_via_second_5214) {
    expectValue5214(twoLibs5214("LDB", "BR5214.RunB(new LDrv5214())"), "207");
}
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_14_1_two_libs_generic_importer_subclass_via_first_5214) {
    expectValue5214(twoLibs5214("EGA", "GA5214.Run(new EG5214<string>())"),
                    "127");
}

// The #5124 sibling: a library L that IMPORTS the method's module calls it
// (the constraint route). An instance of a later module's class without an
// override inherits the base body — 127. The #5124 descriptor-only guard
// aborted here (134); its override sibling (#5124 row Lc) still aborts.
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_11_library_call_site_importer_no_override_5214) {
    Run5214 r = buildChainAndRun5214(
        {"module SA5214L; " + std::string(kTypes5214),
         "module LL5214; import SA5214L; "
         "public static class LibL5214 "
         "  public static method Call(g: GA5214): int "
         "    using var s := new Sq5214(); "
         "    var bx := new Box5214<Sq5214>(s); "
         "    return g.VB(bx); "
         "  end Call; "
         "end LibL5214; ",
         "module SM5214L; import SA5214L; import LL5214; " +
             std::string(kApp5214) + mainOf5214("LibL5214.Call(new Plain5214())")});
    expectValue5214(r, "127");
}

// A root method with a widenable value-typed parameter (`n: int`, §12.1) and
// a WIDENING override in the importer (`n: int?`): the importer installs an
// internal adapter thunk in every sibling's slot, so the library can neither
// recognize the inheriting class nor run the widening override — both abort.
// Pre-fix: a silent 127 for the widening override (8 is correct).
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_12_1_widening_importer_override_traps_5214) {
    const std::string lib =
        "module SW5214; "
        "public interface IW5214 method Area(): int; end IW5214; "
        "public class SqW5214: IW5214 "
        "  public constructor() end constructor; "
        "  public method Area(): int return 7; end Area; "
        "end SqW5214; "
        "public class GW5214 "
        "  public constructor() end constructor; "
        "  public virtual method VW<T>(x: T, n: int): int where T: IW5214 "
        "    return x.Area() + n + 100; "
        "  end VW; "
        "  public static method Run(g: GW5214): int "
        "    using var s := new SqW5214(); "
        "    return g.VW(s, 20); "
        "  end Run; "
        "end GW5214; ";
    const std::string app =
        "module SN5214; import SW5214; "
        "class Wid5214: GW5214 "
        "  public constructor() end constructor; "
        "  public override method VW<T>(x: T, n: int?): int where T: IW5214 "
        "    return x.Area() + 1; "
        "  end VW; "
        "end Wid5214; "
        "class PlainW5214: GW5214 "
        "  public constructor() end constructor; "
        "end PlainW5214; ";
    expectTrap5214(buildChainAndRun5214(
        {lib, app + mainOf5214("GW5214.Run(new Wid5214())")}));
    expectTrap5214(buildChainAndRun5214(
        {lib, app + mainOf5214("GW5214.Run(new PlainW5214())")}));
}

// INTERFACE parent: a library's internal call of a generic interface method
// on a public interface (A.4 `IfaceMethod` admits `where` clauses, but the
// absence of a clause is also valid; without a clause the bodies are
// constants). A known implementer runs its own body (12); an
// implementer declared in the importer aborts — pre-fix it silently ran the
// library implementer's body (12 where 5 is correct).
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_14_2_interface_parent_importer_implementer_traps_5214) {
    const std::string lib =
        "module SI5214; "
        "public interface IV5214 method Area(): int; end IV5214; "
        "public class Sq5214: IV5214 "
        "  public constructor() end constructor; "
        "  public method Area(): int return 7; end Area; "
        "end Sq5214; "
        "public interface IG5214 "
        "  method VI<T>(x: T): int; "
        "end IG5214; "
        "public class AImpl5214: IG5214 "
        "  public constructor() end constructor; "
        "  public method VI<T>(x: T): int return 12; end VI; "
        "end AImpl5214; "
        "public static class RI5214 "
        "  public static method Run(g: IG5214): int "
        "    using var s := new Sq5214(); "
        "    return g.VI(s); "
        "  end Run; "
        "end RI5214; ";
    const std::string app =
        "module SJ5214; import SI5214; "
        "class EImpl5214: IG5214 "
        "  public constructor() end constructor; "
        "  public method VI<T>(x: T): int return 5; end VI; "
        "end EImpl5214; ";
    expectValue5214(buildChainAndRun5214(
                        {lib, app + mainOf5214("RI5214.Run(new AImpl5214())")}),
                    "12");
    expectTrap5214(buildChainAndRun5214(
        {lib, app + mainOf5214("RI5214.Run(new EImpl5214())")}));
}

// INTERFACE parent, fix-forward #5214 (§12.1 / §13 / §14.2, v0.313): an
// importer class that merely INHERITS a library implementer's virtual body
// runs that body — the receiver's itable entry for the interface is the
// library's exported function. On 236b0701 the interface guard was
// descriptor-only and such a correct program aborted (MEASURED by the
// non-author review: abort for 12). An importer class that OVERRIDES the
// body is the later-module override the library cannot specialize, and
// aborts; a GENERIC importer subclass with no override inherits the body
// like the non-generic one.
BEZEL_TEST(xmod_library_internal_virtual_generic_dispatch_5214,
           spec_14_2_interface_parent_importer_inherits_library_implementer) {
    const std::string lib =
        "module SK5214; "
        "public interface IG5214 "
        "  method VI<T>(x: T): int; "
        "end IG5214; "
        "public class AImpl5214: IG5214 "
        "  public constructor() end constructor; "
        "  public virtual method VI<T>(x: T): int return 12; end VI; "
        "end AImpl5214; "
        "public static class RI5214 "
        "  public static method Run(g: IG5214): int "
        "    return g.VI(5); "
        "  end Run; "
        "end RI5214; ";
    const std::string app =
        "module SL5214; import SK5214; "
        "class P5214: AImpl5214 "
        "  public constructor() end constructor; "
        "end P5214; "
        "class Q5214: AImpl5214 "
        "  public constructor() end constructor; "
        "  public override method VI<T>(x: T): int return 99; end VI; "
        "end Q5214; "
        "class GP5214<X>: AImpl5214 "
        "  public constructor() end constructor; "
        "end GP5214; ";
    // inherits the library implementer: the library's body, 12
    expectValue5214(buildChainAndRun5214(
                        {lib, app + mainOf5214("RI5214.Run(new P5214())")}),
                    "12");
    // overrides it in the later module: loud, never the library's 12
    expectTrap5214(buildChainAndRun5214(
        {lib, app + mainOf5214("RI5214.Run(new Q5214())")}));
    // a generic importer subclass with no override inherits the body too
    expectValue5214(
        buildChainAndRun5214(
            {lib, app + mainOf5214("RI5214.Run(new GP5214<string>())")}),
        "12");
}

}  // namespace bezel::module
