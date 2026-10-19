// fix #3211 (§24.3 "Events" anchor / §2.5 "Dependency resolution"; v0.254) —
// the handler lambda parameter of a `Subscribe` on an IMPORTED event was left
// UNTYPED, so any use of it in the handler body failed to resolve. Family M4
// of umbrella #3152; see docs/proposals/papers/member-symbol-audit-2026-08-13.md
// Appendix A.3 row `Calls.cpp:11288` ("UNIFY" with importedMemberType).
//
// SPEC STATUS OF THE SURFACE UNDER TEST. §24 is RETIRED at v0.254 ("the `event`
// member kind no longer exists"; the anchors "are retained for citation
// stability"), and stdlib §6.5's `IEvent<T>`/`EventSource<T>` pair is the
// migration spelling. The PARSER has not caught up to the retirement — it
// still accepts `event` declarations — and the compiler implements the legacy
// member kind end-to-end for LOCAL members and, since #2401/#2407/#2421,
// across a module boundary. This test completes the CROSS-MODULE PARITY of
// that already-committed legacy surface; per CLAUDE.md rule 4 it does not
// resolve #2407's still-open design question (complete the legacy surface vs.
// implement the retirement), which remains for explicit human direction.
//
// WHAT THE DEFECT WAS. `ExprTC::eventHandlerPayloadType`
// (src/sema/ExprTypecheck_Calls.cpp) recovered an event's §24.3 payload type
// from the AST alone:
//
//     auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl);
//     if (!ed || !ed->type) return nullptr;   // "no payload"
//
// `ModuleLoader` never assigns `Symbol::decl` for an imported member (no AST
// is reconstructed for one — the same #503/#2414 limitation Property/Indexer
// carry), so `ed` is null for EVERY imported event and the function reported
// "no payload" — i.e. a unit event, whose handler takes no parameter. The
// handler's `(e)` parameter was then never seeded with a type, and its body
// failed at the first STRICT consumer of `e`.
//
// WHY IT SURFACED AT `+` AND NOT AT `e.N`. A member access on an untyped
// parameter yields an untyped value without a hard error, so the defect is
// SILENT until something demands a known type. §4.6's string-concatenation
// check is such a consumer — which is why #3211's reproduction rejects at the
// `+` with "`+` requires both operands to be `string`" — but it is NOT the
// only consequence. The same untypedness is also a false ACCEPT:
//   - a `e.NoSuchField3211` member access in the handler body fails to
//     resolve, but with an `internal` "argument failed to lower" warning
//     (tests/codegen exit 0), not the clean `no member` the single-module
//     twin reports;
//   - an `e` passed to a method whose parameter is a DIFFERENT record type
//     fails to be checked at sema, then codegen — which DOES know the payload
//     type via #2421 — emits mismatched IR that the IR verifier catches as
//     "Call parameter type does not match function signature". If two record
//     types had identical LLVM layout the IR verifier would NOT catch it and
//     the program would silently compute on the wrong type.
//
// MEASURED ON `main` @ 8b7e458 (hand-probe via the real `bezelc` AOT path,
// each row's binary re-derived on HEAD before the fix):
//
//   T1   "got=" + e.N.ToString()   twin COMPILES (exit 0) | imported REJECTED:
//                                  "`+` requires both operands to be `string`
//                                  … use interpolation … or .ToString() (§4.6)"
//                                  — the FALSE REJECT of #3211.
//   T2   e.NoSuchField3211         twin REJECTED          | imported ACCEPTED
//                                  ("internal: argument failed to lower"
//                                  exit 0) — the same untypedness is ALSO a
//                                  false ACCEPT.
//   T3   HelpP12.TakeOther(e)      twin REJECTED (§5.1)   | imported MISCOMPILED
//                                  (IR verification: "Call parameter type
//                                  does not match function signature") — the
//                                  most dangerous consequence, because it
//                                  would silently produce wrong results when
//                                  the two record types share LLVM layout.
//   T4   no-payload `event Ping;`  twin COMPILES          | imported COMPILES
//                                  — the control that the fix must not break.
//
// THE FIX. Mirror the decl-null fallback the CODEGEN side of this same feature
// has carried since #2421 (`Emitter::eventPayloadType`,
// src/codegen/LlvmBackend_Calls2.cpp:2667) into sema: read the wire-carried
// payload type from `SemaContext::importedMemberType` when `es->decl` is null,
// treating a Unit type — the exporter's encoding of an omitted `: T` — as "no
// payload", exactly as the local `!ed->type` path does. The side table is
// populated by ModuleLoader.cpp:1461 (`fieldTypes_[e] = convert(mm.type)`) from
// `MetaMember::type`, which ModuleMetadataBuilder.cpp:1308 writes from the
// exporter's `ast::EventDecl::type`. The asymmetry this fix removes is that
// codegen could already LOWER a cross-module payload handler that sema
// rejected first.
//
// DIRECT RUNTIME MEASUREMENT (BEZEL_PROBE_3211, stripped before commit):
// imported `event Ev: PayP12` → importedMemberType=NONNULL kind=Record(2)
// imported `event Ping;`     → importedMemberType=NONNULL kind=Unit(3)
// Both confirm the side table is populated for events (not assumed from the
// audit). The kind values drive the Unit-vs-payload distinction the fix has
// to honor.
//
// ⚠ THE HANDLER DOES NOT OBSERVABLY FIRE for a DIRECT (non-interface) event
// raised through the bare-call form `Ev(payload)` — the single-module twin
// compiles, runs, exits 0 and prints NOTHING. That is a SEPARATE, non-import
// defect in the §24.3 raise path (it reproduces with no module boundary
// anywhere), so every row here asserts COMPILE-or-REJECT-and-exit-not-0
// rather than an observed payload value. Filed separately (see commit
// message). Rows T2/T3 are what keep "it compiles/rejects" from being a weak
// assertion: they fail if the parameter is typed as anything other than the
// true payload type.
//
// T5 pins a separate §24.3 rule the fix interacts with: an event read on an
// instance of a class OTHER THAN its declaring class has static type
// `IEvent<T>`, where only `.Subscribe` is available — `.Add` and `.Remove`
// belong to the declaring class's own access. Pre-fix the imported form
// rejected `.Add` correctly AND emitted the spurious §4.6 string-concat
// complaint below it; the post-fix form rejects `.Add` cleanly without the
// spurious §4.6 noise. (T5 is also why this test never had a `.Add` row
// that the fix had to type: the `.Add` access path is structurally blocked
// from this site.)

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3211 {
    std::string path;
    explicit TempFile3211(std::string p) : path(std::move(p)) {}
    ~TempFile3211() { std::remove(path.c_str()); }
};

void writeSource3211(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The declarations under test — written ONCE and reused verbatim by both the
// dependency and the single-module twin, so the only difference between an
// imported row and its control is the module boundary itself.
//
//   PayP12   — #3211's payload record.
//   OtherP12 — an UNRELATED record, for T3's wrong-type discriminator.
//   SrcP12   — the publisher. `Ev` carries a payload; `Ping` deliberately
//              omits `: T` (the §24 unit form) so T4 can pin that the
//              exporter's Unit encoding is still read back as "no payload".
//   HelpP12  — a strict consumer of `OtherP12`, used by T3.
const char* kDecls3211 =
    "public record PayP12(N: int); "
    "public record OtherP12(M: int); "
    "public class SrcP12 "
    "  public event Ev: PayP12; "
    "  public event Ping; "
    "  public constructor() end constructor; "
    "  public method Fire() "
    "    Ev(new PayP12(5)); "
    "  end Fire; "
    "end SrcP12; "
    "public class HelpP12 "
    "  public static method TakeOther(o: OtherP12): int => o.M; "
    "end HelpP12;";

std::string libSource3211() {
    return std::string("module EvtParamLib3211; ") + kDecls3211;
}

std::string appSource3211(const std::string& body, const std::string& tag) {
    return "module EvtParamApp3211" + tag + "; import EvtParamLib3211; "
           "public class CP12" + tag + " " + body + " end CP12" + tag + ";";
}

// The single-module twin: the same declarations, the same body, no `import`.
std::string sameModuleProgram3211(const std::string& body,
                                  const std::string& tag) {
    return "module EvtParamCtl3211" + tag + "; " + kDecls3211 +
           " public class CP12" + tag + " " + body + " end CP12" + tag + ";";
}

// Compile the dependency, compile the app against it, run the app in a SEPARATE
// process (per tests/README.md). Returns the child's exit status, or:
//   -1 compile/link/run failure (the app did not build)
//   -2 the dependency itself could not be produced
// `capturedStderr`, when non-null, receives the app compile's stderr — the
// only way to assert on diagnostic TEXT.
int runImported3211(const std::string& body, const std::string& tag,
                    std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3211_" + tag + "_" + unique_stem("");

    TempFile3211 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3211 aObj((dir / (stem + "_a.o")).string());
    writeSource3211(aSrc.path, libSource3211());
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3211 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3211 bExe((dir / (stem + "_b")).string());
    TempFile3211 bErr((dir / (stem + "_b.err")).string());
    writeSource3211(bSrc.path, appSource3211(body, tag));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = driver.compileFile(bSrc.path, bExe.path, deps).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(bErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = driver.compileFile(bSrc.path, bExe.path, deps).success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(bExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);  // 1011 = SIGSEGV
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Compile and run a SINGLE-module program — the differential control twin.
// Same return convention as runImported3211 minus the -2 case.
int runSingleModule3211(const std::string& src, const std::string& tag,
                        std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3211ctl_" + tag + "_" + unique_stem("");
    TempFile3211 sSrc((dir / (stem + ".bzl")).string());
    TempFile3211 sExe((dir / stem).string());
    TempFile3211 sErr((dir / (stem + ".err")).string());
    writeSource3211(sSrc.path, src);

    bezel::Driver driver;
    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(sErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = driver.compileFile(sSrc.path, sExe.path, {}).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(sErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = driver.compileFile(sSrc.path, sExe.path, {}).success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(sExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(sExe.path.c_str(), sExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool mentions3211(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ===========================================================================
// T0 — ANTI-VACUITY GUARD. Every row below skips when the dependency cannot
// be produced (-2), which would make the whole file pass without testing
// anything. This row asserts the dependency DOES build, so a broken library —
// e.g. if the parser's not-yet-retired `event` acceptance (§24, v0.254) were
// removed — fails LOUDLY here instead of silently voiding the file.
// ===========================================================================
BEZEL_TEST(xmod_event_handler_param_type_3211, t0_dependency_library_builds) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3211dep_");
    TempFile3211 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3211 aObj((dir / (stem + "_a.o")).string());
    writeSource3211(aSrc.path, libSource3211());
    bezel::Driver driver;
    BEZEL_EXPECT_TRUE(driver.compileObject(aSrc.path, aObj.path).success);
    BEZEL_EXPECT_TRUE(std::ifstream(aObj.path).good());
}

// ===========================================================================
// T1 — #3211's OWN REPRODUCTION (PRIMARY regression). The handler parameter
// of a Subscribe on an IMPORTED event must be typed with the event's §24.3
// payload type, so `e.N.ToString()` resolves to `string` and §4.6's
// concatenation check is satisfied. PRE-fix the imported form was REJECTED
// at the `+`; POST-fix it compiles and exits 0.
// ===========================================================================
BEZEL_TEST(xmod_event_handler_param_type_3211,
           t1_imported_event_handler_param_typed_24_3) {
    const std::string body =
        "  public static method Main(): int "
        "    using var s := new SrcP12(); "
        "    using var sub := s.Ev.Subscribe(lambda(e) => "
        "      Console.WriteLine(\"got=\" + e.N.ToString())); "
        "    s.Fire(); "
        "    return 0; "
        "  end Main;";
    int rc = runImported3211(body, "t1");
    if (rc == -2) return;  // dependency unavailable — T0 reports this loudly
    BEZEL_EXPECT_EQ(rc, 0);
}

// ===========================================================================
// T1b — the CONTROL twin for T1: character-identical modulo the module
// boundary. It compiled before the fix and must still compile after. Its
// role is to prove T1's rejection was caused by the boundary and by nothing
// else in the program.
// ===========================================================================
BEZEL_TEST(xmod_event_handler_param_type_3211,
           t1b_same_module_event_handler_param_typed_24_3) {
    const std::string body =
        "  public static method Main(): int "
        "    using var s := new SrcP12(); "
        "    using var sub := s.Ev.Subscribe(lambda(e) => "
        "      Console.WriteLine(\"got=\" + e.N.ToString())); "
        "    s.Fire(); "
        "    return 0; "
        "  end Main;";
    BEZEL_EXPECT_EQ(runSingleModule3211(sameModuleProgram3211(body, "t1b"),
                                        "t1b"),
                    0);
}

// ===========================================================================
// T2 — THE PARAMETER IS TYPED TO THE *CORRECT* TYPE, discriminator 1. A
// handler that reads a field the payload does not have must be REJECTED, on
// both sides of the boundary. This is the row that keeps T1's "it compiles"
// honest: a fix that typed `e` as some permissive unknown (or as `Object`)
// would make T1 pass while silently ACCEPTING this program. Pre-fix the
// imported form ACCEPTED it (with an "internal" lowering-warning, exit 0) —
// the untypedness is a false ACCEPT as well as #3211's false reject.
// ===========================================================================
BEZEL_TEST(xmod_event_handler_param_type_3211,
           t2_imported_handler_unknown_payload_field_rejected_24_3) {
    const std::string body =
        "  public static method Main(): int "
        "    using var s := new SrcP12(); "
        "    using var sub := s.Ev.Subscribe(lambda(e) => "
        "      Console.WriteLine(e.NoSuchField3211.ToString())); "
        "    s.Fire(); "
        "    return 0; "
        "  end Main;";
    int impRc = runImported3211(body, "t2");
    if (impRc == -2) return;
    int ctlRc = runSingleModule3211(sameModuleProgram3211(body, "t2c"),
                                    "t2c");
    BEZEL_EXPECT_EQ(ctlRc, -1);  // the twin rejects it …
    BEZEL_EXPECT_EQ(impRc, -1);  // … and so must the imported form
}

// ===========================================================================
// T3 — THE PARAMETER IS TYPED TO THE *CORRECT* TYPE, discriminator 2. Passing
// the payload to a method that expects an UNRELATED record must be rejected
// on both sides (§5.1: no implicit conversion). T2 pins "not a permissive
// member-lookup"; this pins "not a permissive conversion" — together they
// exclude every way `e` could carry a type other than `PayP12` and still let
// T1 pass.
//
// Pre-fix the imported form MISCOMPILED: sema did not check the argument
// type (no `e` type to check against), then codegen — which DOES know the
// payload type via #2421 — emitted mismatched IR that the LLVM verifier
// caught as "Call parameter type does not match function signature". Post-fix
// sema rejects at the §5.1 conversion, matching the twin.
//
// This row is what keeps the bug from being a SILENT miscompile when two
// record types happen to share LLVM layout — the IR verifier would not catch
// it then, and the program would compute on the wrong type.
// ===========================================================================
BEZEL_TEST(xmod_event_handler_param_type_3211,
           t3_imported_handler_payload_wrong_type_rejected_5_1) {
    const std::string body =
        "  public static method Main(): int "
        "    using var s := new SrcP12(); "
        "    using var sub := s.Ev.Subscribe(lambda(e) => "
        "      Console.WriteLine(HelpP12.TakeOther(e).ToString())); "
        "    s.Fire(); "
        "    return 0; "
        "  end Main;";
    int impRc = runImported3211(body, "t3");
    if (impRc == -2) return;
    int ctlRc = runSingleModule3211(sameModuleProgram3211(body, "t3c"),
                                    "t3c");
    BEZEL_EXPECT_EQ(ctlRc, -1);  // the twin rejects it …
    BEZEL_EXPECT_EQ(impRc, -1);  // … and so must the imported form
}

// ===========================================================================
// T4 — THE UNIT (no-payload) EVENT MUST STAY UNIT. `event Ping;` omits `: T`;
// the exporter encodes that as a Unit MetaType, so a fallback that returned the
// side-table type RAW would hand the handler a phantom parameter and break
// this row. The fix must read Unit as "no payload" — exactly what the local
// `!ed->type` path does, and what codegen's own #2421 fallback already does.
// This row passed before the fix and must keep passing: it is the regression
// guard ON the fix.
// ===========================================================================
BEZEL_TEST(xmod_event_handler_param_type_3211,
           t4_imported_unit_event_handler_takes_no_param_24_3) {
    const std::string body =
        "  public static method Main(): int "
        "    using var s := new SrcP12(); "
        "    using var sub := s.Ping.Subscribe(lambda() => "
        "      Console.WriteLine(\"ping\")); "
        "    return 0; "
        "  end Main;";
    int rc = runImported3211(body, "t4");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 0);
}

// ===========================================================================
// T5 — §24.3 ACCESS RULE INTERACTS WITH THE FIX. An event read on an instance
// of a class OTHER THAN its declaring class has static type `IEvent<T>`, where
// only `.Subscribe` is available — `.Add` and `.Remove` belong to the
// declaring class's own access. Pre-fix the imported form rejected `.Add`
// correctly AND emitted the spurious §4.6 string-concat complaint below it
// (the untyped handler); post-fix the §24.3 rejection stands alone, with no
// §4.6 noise.
//
// This row keeps the fix from accidentally REGRESSING into a state where the
// §24.3 rejection is masked by a downstream §4.6 complaint, so a future
// regression in either rule would surface as the spurious diagnostic rather
// than go undetected.
// ===========================================================================
BEZEL_TEST(xmod_event_handler_param_type_3211,
           t5_imported_add_24_3_rejection_clean_no_4_6_noise) {
    const std::string body =
        "  public static method Main(): int "
        "    using var s := new SrcP12(); "
        "    s.Ev.Add(lambda(e) => "
        "      Console.WriteLine(\"add=\" + e.N.ToString())); "
        "    s.Fire(); "
        "    return 0; "
        "  end Main;";
    std::string err;
    int rc = runImported3211(body, "t5", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);  // .Add on a non-declaring-class instance
    BEZEL_EXPECT_TRUE(
        mentions3211(err, "is accessed from outside its declaring class"));
    BEZEL_EXPECT_TRUE(!mentions3211(err, "requires both operands to be"));
}

// ===========================================================================
// T6 — THE DIAGNOSTIC TEXT of #3211's false reject is gone for T1's program.
// T1 asserts the program builds; this row asserts the compiler no longer
// emits the §4.6 concatenation complaint for it — pinning the specific
// misdescribing message the issue reported, so a future regression that
// rejects for a DIFFERENT reason is not mistaken for this one staying fixed.
// ===========================================================================
BEZEL_TEST(xmod_event_handler_param_type_3211,
           t6_imported_handler_no_false_string_concat_diagnostic_4_6) {
    const std::string body =
        "  public static method Main(): int "
        "    using var s := new SrcP12(); "
        "    using var sub := s.Ev.Subscribe(lambda(e) => "
        "      Console.WriteLine(\"got=\" + e.N.ToString())); "
        "    s.Fire(); "
        "    return 0; "
        "  end Main;";
    std::string err;
    int rc = runImported3211(body, "t6", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_TRUE(!mentions3211(err, "requires both operands to be"));
}

}  // namespace bezel::module
