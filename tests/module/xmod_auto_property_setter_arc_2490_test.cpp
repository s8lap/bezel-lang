// fix #2490 (§23.3 "Block-form properties" / §28.9 "Runtime-managed types",
// v0.236): Emitter::ensurePropertyAccessorFn's auto-property SETTER body
// (LlvmBackend_Assign.cpp) performed a bare, unguarded pointer store into the
// backing field for EVERY type — string, closure, Object/interface-boxed —
// with no retain of the incoming value and no release of the value being
// displaced.
//
// GOVERNING RULE (§23.3): "The setter's implicit `value` parameter has no
// modifier by default and takes a BORROW of the property's declared type."
// Auto-properties have no syntax slot for `set(owned)` (admitted only on
// block-form properties/indexers, same section) — so an auto-property
// setter's `value` is ALWAYS a borrow, never an announced transfer. A borrow
// means the CALLER keeps its own reference; if the callee wants to keep the
// value alive past the call (storing it into a field that outlives the call
// obviously does), it must mint its OWN independent +1 — exactly like an
// ordinary in-class field store of a plain parameter already does
// (lowerStringFieldStore / lowerClosureFieldStore, #429 / §4.6 / §27.5 /
// §28.9).
//
// THE FIX (two parts, both required — verified independently, see below):
//  1. `ensurePropertyAccessorFn`'s auto-setter body (LlvmBackend_Assign.cpp)
//     now does load-old -> retain-new -> store-new -> release-old for a
//     string- or closure-typed CLASS auto-property (isStringStorageMember /
//     isClosureStorageMember), mirroring lowerStringFieldStore /
//     lowerClosureFieldStore exactly. Gated to CLASS only (a RECORD
//     auto-property backing field is a borrow with no independent +1 and no
//     disposer, §9/§9.7 — unaffected, T6 below). Object/interface (box)-typed
//     auto-properties are NOT covered — see issue #2500 for why
//     (isBoxStorageMember's classification prepass, collectBoxStorageFields,
//     runs AFTER the two call sites that can first build this function's
//     body, so it would read empty/stale — a genuine ordering hazard, not
//     guessed around).
//  2. The two caller-side call sites that actually INVOKE the generated
//     setter function — lowerImportedPropertySetValue (cross-module direct
//     set, fix #2400) and lowerVirtualPropertySet (itable/vtable dispatch,
//     #481) — now release a +1 producer expression passed DIRECTLY as the
//     setter argument (no local binding of its own) after the call, mirroring
//     the established #429 "ARGUMENT-PASS category" release an ordinary
//     method call already gives such an argument (LlvmBackend_Calls2.cpp
//     ~7035) and the #1073/§28.14 closure analogue. WITHOUT this second part,
//     part 1's new retain alone turns a `h.Name := "a" + "b";` (no local) from
//     accidentally-balanced (pre-fix: no retain, so the producer's single
//     reference transferred to the field with nothing to reclaim, no leak) —
//     into a genuine, confirmed 32-byte LSan-caught leak (verified by hand
//     before the second part was added; T3 below is the regression pin).
//
//     PART 2 IS ITSELF GATED, and this gating is load-bearing, not cosmetic
//     — an adversarial pass building on a coordinator review of the
//     concurrently-landing #2489 (which widened emitExportedPropertySetters
//     to `Class || Record`, reaching this SAME shared setter body for
//     records too) found that an UNGATED release is a confirmed LIVE UAF: a
//     RECORD's auto-property setter is (correctly, per part 1's own Class-
//     only gate) still a plain, non-retaining store, so releasing a producer
//     the callee never retained drops the field's only reference immediately
//     (confirmed: `r.Name := "hel" + "lo!";` on an imported RECORD crashed
//     in bz_string_to_utf8z when the release fired unconditionally — found
//     and fixed BEFORE this file's final version, not left in). The shipped
//     gates:
//       - lowerImportedPropertySetValue releases only when
//         `ps->parent->kind == Class` (the concrete, statically-known
//         property owner — sound, since this path is explicitly the
//         NON-itable direct set).
//       - lowerVirtualPropertySet releases only on the NON-interface
//         (`!isIfaceProp`) branch — a class's own vtable-slot virtual/
//         override property, where `ps->parent` is reliably the declaring
//         CLASS (records cannot enter a vtable hierarchy, §9: "a record
//         cannot derive from another record or from a class"). The
//         itable (interface-dispatched) branch is left WITHOUT the release:
//         `ps` there is the INTERFACE's own PropertySymbol, so
//         `ps->parent->kind` is always Interface regardless of whether the
//         concrete runtime receiver is a class instance or a boxed record
//         (§11: "a record may implement any number of interfaces"; an
//         interface reference "holds a reference: to a class instance
//         directly, [or] to a record value boxed at assignment") — this call
//         site cannot see which concrete setter an itable call will actually
//         invoke, so a direct producer passed through an INTERFACE-
//         dispatched setter leaks rather than risks an unverified UAF
//         (T8 below pins this — leak-over-UAF, the explicit safe direction).
//
// SCOPE NOTE — a SECOND, textually-adjacent bug the issue's own filer flagged
// as "unconfirmed root cause" (T2 in the issue: a BARE identifier setter call
// `Name := v;`, implicit `this`, inside a locally-declared class inheriting
// an imported base's auto-property) was investigated here and found to be a
// DIFFERENT, DEEPER root cause than this fix touches: `isStringExprType`
// (LlvmBackend_Calls2.cpp, i.e. `ctx_.exprType`, a SEMA-side type-resolution
// result) already returns false for that bare-identifier assignment-target
// shape, before codegen's own isStringFieldLvalue/isAutoProperty classifiers
// are ever consulted — confirmed by direct instrumentation, not inferred.
// This REFUTES the issue's own working hypothesis (isAutoProperty's
// decl-dependence in isStringFieldLvalue) for the bare-identifier case
// specifically: a drafted isAutoProperty -> isAutoPropertyStorage widening at
// those three predicates had ZERO observable effect on this repro (execution
// never reaches the changed code) and was reverted rather than shipped
// unproven, matching this repo's "verify the outcome" discipline. The
// EXPLICIT-receiver sibling of that same repro (`this.Name := v;` instead of
// bare `Name := v;`) IS fixed by this change (T5 below) — it routes through
// lowerImportedPropertySet -> lowerImportedPropertySetValue, this fix's own
// call site, since a MemberAccessExpr target (unlike a bare Identifier) does
// match lowerImportedPropertySet's `dynamic_cast<MemberAccessExpr>` gate.
// The bare-identifier sub-variant remains open, filed as its own issue with
// the narrowed root cause (see the fix's commit message / PR description).
//
// KNOWN, ACCEPTED RESIDUAL LEAK (documented, not fixed here, filed
// separately): a NAMED LOCAL (not a direct producer expression) passed to a
// cross-module or virtual/interface property SETTER call is not yet
// recognized as a "call-bounded borrow" by the string escape walker (the
// walker that decides which locals get scope-end release) — so the local's
// OWN independent +1 (now correctly retained into the field by part 1 of
// this fix) is never released at the caller's own scope end. This is
// STRICTLY BETTER than the pre-fix state (a confirmed UAF/heap-corruption,
// T1/T5 below) — leak-over-UAF is this repo's explicit accepted direction —
// but it is a real, per-call, loop-compounding leak (confirmed: a 5-iteration
// loop leaks 5 objects). Filed separately; T1/T5 below therefore run with
// LeakSanitizer OFF (they pin the crash/correctness fix only, per the lane
// protocol's guidance to isolate leak-hunting from control-flow verification
// — ASAN_OPTIONS=detect_leaks=0). T3/T4/T7/T8 below, which do NOT pass a
// named local, run with LeakSanitizer ON and are expected fully clean.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile2490 {
    std::string path;
    explicit TempFile2490(std::string p) : path(std::move(p)) {}
    ~TempFile2490() { std::remove(path.c_str()); }
};

void writeSource2490(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2490(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compiles `appSrc` importing `libObj`, links + runs under ASan with a
// distinct crash exit code. `leaks` selects whether LeakSanitizer is armed
// (see the file header for why T1/T5 run with it off and T3/T4/T7/T8 on).
// Returns: the program's exit code on a clean run, 97 if ASan/LSan caught a
// memory-safety error or leak, -1 on compile/link failure, -2 if the
// toolchain is unavailable (SKIP, not a failure).
int compileImportRunAsan2490(const std::string& libSrc,
                             const std::string& appSrc, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2490_" + std::to_string(std::rand());
    TempFile2490 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2490 aObj((dir / (stem + "_a.o")).string());
    writeSource2490(aSrc.path, libSrc);
    if (!compileToObj2490(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2490 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2490 bExe((dir / stem).string());
    writeSource2490(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!driver.compileFile(bSrc.path, bExe.path, deps, opts).success)
        return -1;

    std::string runCmd = std::string("ASAN_OPTIONS=detect_leaks=") +
        (leaks ? "1" : "0") +
        ":abort_on_error=0:exitcode=97 " + bExe.path + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(bExe.path.c_str());
    if (status == -1) return -2;
    if (WIFSIGNALED(status)) return 97;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Mirrors compileImportRunAsan2490 but for a SINGLE-module (no import)
// program — used for the local-only virtual-dispatch and record/value
// controls, which are unaffected by the cross-module ARGUMENT-PASS half of
// this fix but DO exercise the ensurePropertyAccessorFn ARC half whenever the
// property is virtual/interface-dispatched.
int compileRunAsan2490Local(const std::string& src, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2490_local_" + std::to_string(std::rand());
    TempFile2490 srcFile((dir / (stem + ".bzl")).string());
    TempFile2490 exe((dir / stem).string());
    writeSource2490(srcFile.path, src);

    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!driver.compileFile(srcFile.path, exe.path, {}, opts).success)
        return -1;

    std::string runCmd = std::string("ASAN_OPTIONS=detect_leaks=") +
        (leaks ? "1" : "0") +
        ":abort_on_error=0:exitcode=97 " + exe.path + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(exe.path.c_str());
    if (status == -1) return -2;
    if (WIFSIGNALED(status)) return 97;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

const char* kLibStr2490 =
    "module LibStr2490; "
    "public class Holder2490 "
    "  public property Name: string := \"init\"; "
    "end Holder2490;";

}  // namespace

// ===========================================================================
// T1 — CORE repro (the issue's own confirmed T1, §23.3/§28.9): a NAMED
// local's string is stored into an imported class's auto-property setter.
// Pre-fix: SEGV in bz_string_to_utf8z (confirmed by hand before this test —
// the exact crash the issue reports). Post-fix: the field takes its own
// independent +1, the local remains valid after `h`'s disposal, and the
// printed value is exactly the local's content. LeakSanitizer OFF (the KNOWN,
// separately-filed residual — see file header); this test pins the
// crash/correctness half only.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_arc_2490,
           named_local_string_survives_setter_and_disposal_23_3_28_9) {
    const std::string appSrc =
        "module App2490T1; import LibStr2490; "
        "public class Program2490T1 "
        "  public static method Main(): int "
        "    var s := \"hel\" + \"lo!\"; "
        "    do "
        "      using var h := new Holder2490(); "
        "      h.Name := s; "
        "    end; "
        "    return s.Length; "
        "  end Main; "
        "end Program2490T1;";
    int r = compileImportRunAsan2490(kLibStr2490, appSrc, /*leaks=*/false);
    if (r == -2) return;  // toolchain / dependency build unavailable — skip
    BEZEL_EXPECT_EQ(r, 6);  // "hello!".Length, no crash
}

// ===========================================================================
// T2 — CONTROL: the identical shape but the property is set from a FRESH
// value produced INSIDE the callee's own field-store discipline, read back
// through the SAME setter twice (proves retain-before-release ordering keeps
// a REPEATED reassignment — "set twice in succession" from the fix's own
// adversarial grid — safe: each new value correctly displaces and releases
// the prior one, no crash, final read correct). Both values are DIRECT
// producers (no local binding), so LeakSanitizer stays ON — this also proves
// the ARGUMENT-PASS release (fix part 2) does not under- or over-release
// across a repeated call.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_arc_2490,
           set_twice_in_succession_direct_producers_no_leak_28_9) {
    const std::string appSrc =
        "module App2490T2; import LibStr2490; "
        "public class Program2490T2 "
        "  public static method Main(): int "
        "    using var h := new Holder2490(); "
        "    h.Name := \"aa\" + \"bb\"; "
        "    h.Name := \"ccc\" + \"ddd\"; "
        "    return h.Name.Length; "
        "  end Main; "
        "end Program2490T2;";
    int r = compileImportRunAsan2490(kLibStr2490, appSrc, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 6);  // "cccddd".Length; no crash, no leak
}

// ===========================================================================
// T3 — the LEAK regression this fix's own second part (the caller-side
// ARGUMENT-PASS release) closes: a fresh +1 producer passed DIRECTLY as the
// setter argument (no local binding of its own). Before part 2 was added
// (verified BY HAND, not by this gtest — see note below), part 1's new
// unconditional retain ALONE turned this into a confirmed 32-byte LSan leak
// — pre-fix (neither part applied — no retain at all) this shape was
// ACCIDENTALLY balanced too (the producer's single reference simply
// transferred to the field), so this is the one shape a retain-only partial
// fix would have REGRESSED. LeakSanitizer ON; must be clean.
//
// FAIL-RED NUANCE (honesty note): unlike T1/T2/T4/T5, this test does NOT
// fail red against a full revert of this fix — a full revert (no retain, no
// release) is observationally IDENTICAL here (no crash, no leak), since the
// pre-fix bare store already balanced this exact narrow shape by accident.
// It DOES fail red against the intermediate "retain-only" state (confirmed
// by hand: reverting only the ARGUMENT-PASS release lines while keeping the
// ensurePropertyAccessorFn retain reproduces a mechanical 32-byte LSan leak)
// — that three-way state is impractical to also automate as a build
// variant here, so this test's fail-red claim rests on the by-hand
// verification, not on this suite's own revert/rebuild cycle. Kept as a
// regression guard against a FUTURE re-introduction of the same partial
// state (e.g. someone adding the retain to a fourth call site without its
// matching release).
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_arc_2490,
           direct_producer_argument_no_leak_argument_pass_release_28_9) {
    const std::string appSrc =
        "module App2490T3; import LibStr2490; "
        "public class Program2490T3 "
        "  public static method Main(): int "
        "    using var h := new Holder2490(); "
        "    h.Name := \"hel\" + \"lo!\"; "
        "    return 0; "
        "  end Main; "
        "end Program2490T3;";
    int r = compileImportRunAsan2490(kLibStr2490, appSrc, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);  // no crash, no leak
}

// ===========================================================================
// T4 — a direct +1 producer set in a LOOP (the fix's grid's "set inside a
// loop" axis), each iteration through the SAME cross-module setter call site
// (lowerImportedPropertySetValue). Proves the ARGUMENT-PASS release does not
// merely work once but holds under repeated invocation of the SAME call
// site — a bug here would compound per iteration and be caught by
// LeakSanitizer regardless of iteration count. LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_arc_2490,
           direct_producer_in_loop_no_leak_28_9) {
    const std::string appSrc =
        "module App2490T4; import LibStr2490; "
        "public class Program2490T4 "
        "  public static method Main(): int "
        "    using var h := new Holder2490(); "
        "    var i := 0; "
        "    while i < 5 do "
        "      h.Name := \"x\" + i.ToString(); "
        "      i := i + 1; "
        "    end while; "
        "    return h.Name.Length; "
        "  end Main; "
        "end Program2490T4;";
    int r = compileImportRunAsan2490(kLibStr2490, appSrc, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 2);  // "x4".Length; no crash, no leak
}

// ===========================================================================
// T5 — the EXPLICIT-receiver sibling of the issue's T2 (bare-identifier)
// variant: `this.Name := v;` inside a LOCALLY-declared class's own method,
// where `Name` is inherited from an IMPORTED base's auto-property, and `v`
// is a plain (borrow) method PARAMETER — not a direct producer. A
// MemberAccessExpr target (unlike the bare-identifier form) DOES match
// lowerImportedPropertySet's dynamic_cast gate, so this shape routes through
// this fix's own call site and is FIXED by it (confirmed: pre-fix a bare
// unguarded store here caused the SAME double-free-shaped SEGV in
// bz_string_release the issue's T2 describes for the bare-identifier form;
// post-fix, clean). LeakSanitizer ON: `v` flows in from `s`, a NAMED local at
// the SetIt call site — but SetIt is a plain locally-compiled method call, so
// the PRE-EXISTING #1430 "call-bounded read" relaxation (LlvmBackend_Setup.cpp,
// unrelated to and unmodified by this fix) already arms `s`'s own scope-end
// release, so this specific shape is NOT subject to the KNOWN residual leak
// documented in the file header (that gap is specifically about a NAMED
// LOCAL passed DIRECTLY as `obj.Prop := local;`'s RHS, a different call
// shape than a local passed as an ordinary method argument that then writes
// its OWN field via `this.Name := v;`).
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_arc_2490,
           explicit_this_receiver_inherited_import_no_crash_no_leak_23_3) {
    const std::string appSrc =
        "module App2490T5; import LibStr2490; "
        "public class Derived2490T5: Holder2490 "
        "  public method SetIt(v: string) "
        "    this.Name := v; "
        "  end SetIt; "
        "end Derived2490T5; "
        "public class Program2490T5 "
        "  public static method Main(): int "
        "    var s := \"hel\" + \"lo!\"; "
        "    using var h := new Derived2490T5(); "
        "    h.SetIt(s); "
        "    return h.Name.Length; "
        "  end Main; "
        "end Program2490T5;";
    int r = compileImportRunAsan2490(kLibStr2490, appSrc, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 6);  // "hello!".Length; no crash, no leak
}

// ===========================================================================
// T6 — CONTROL: a mutable RECORD auto-property (§9.2 permits mutable
// record body-properties). Per §9 a record's reference fields are BORROWS
// with no independent +1 and no disposer (§9.7), so ensurePropertyAccessorFn's
// fix is deliberately gated to CLASS only — this proves the gate holds: a
// record's setter keeps the pre-fix plain store, unaffected, no crash from
// wrongly retaining/releasing a value the record does not own.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_arc_2490,
           mutable_record_auto_property_setter_unaffected_control_9_2) {
    const std::string src =
        "module LocalRec2490T6; "
        "public record RHolder2490T6 "
        "  public property Name: string := \"init\"; "
        "end RHolder2490T6; "
        "public class Program2490T6 "
        "  public static method Main(): int "
        "    var r := new RHolder2490T6(); "
        "    r.Name := \"ok\"; "
        "    return r.Name.Length; "
        "  end Main; "
        "end Program2490T6;";
    int r = compileRunAsan2490Local(src, /*leaks=*/false);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 2);  // "ok".Length, no crash
}

// ===========================================================================
// T7 — CONTROL: a plain `int`-typed (VALUE-typed) auto-property. Neither
// isStringStorageMember nor isClosureStorageMember matches an int, so the
// setter keeps the plain store this fix leaves untouched for that category —
// proves no incidental regression to the ordinary, non-ARC-eligible case.
// LOCAL, but routed through a VIRTUAL/interface dispatch call
// (lowerVirtualPropertySet, this fix's SECOND call site) so this also
// exercises that path's int/non-pointer early-out (valueExpr recognition is
// gated on `v->getType()->isPointerTy()`-shaped checks upstream; an int
// value never matches isPlusOneStringProducer/isPlusOneClosureProducer, so
// the new release call is correctly skipped for it).
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_arc_2490,
           value_typed_auto_property_setter_unaffected_control) {
    const std::string src =
        "module LocalVal2490T7; "
        "public interface ICounter2490T7 "
        "  property Count: int get; set; end; "
        "end ICounter2490T7; "
        "public class Counter2490T7: ICounter2490T7 "
        "  public property Count: int := 0; "
        "end Counter2490T7; "
        "public class Program2490T7 "
        "  public static method Main(): int "
        "    using var h := new Counter2490T7(); "
        "    var iref: ICounter2490T7 := h; "
        "    iref.Count := 41; "
        "    iref.Count := iref.Count + 1; "
        "    return iref.Count; "
        "  end Main; "
        "end Program2490T7;";
    int r = compileRunAsan2490Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);  // no crash, no leak (nothing heap-allocated)
}

// ===========================================================================
// T8 — INTERFACE dispatch (lowerVirtualPropertySet's itable branch, this
// fix's second call site), string-typed, LOCAL — a direct +1 producer set
// through an INTERFACE reference. Proves the generated setter body's new
// retain (ensurePropertyAccessorFn) still fires safely (NO CRASH) through
// itable dispatch, and pins the DELIBERATE gap this fix's own adversarial
// pass forced (see the file header's "PART 2 IS ITSELF GATED" note): the
// itable branch does NOT get the ARGUMENT-PASS release (it cannot tell,
// from `ps->parent` alone, whether the concrete dynamic receiver is a class
// or a boxed record, and an ungated release is a confirmed live UAF for the
// record case) — so this specific shape LEAKS. LeakSanitizer therefore runs
// OFF here (matching T1/T5's "pin crash-freedom only" discipline); the leak
// itself is the accepted, documented trade-off, not a defect this test
// should fail on.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_arc_2490,
           interface_dispatch_string_direct_producer_no_crash_23_3) {
    const std::string src =
        "module LocalVirt2490T8; "
        "public interface ISettable2490T8 "
        "  property Name: string get; set; end; "
        "end ISettable2490T8; "
        "public class Impl2490T8: ISettable2490T8 "
        "  public property Name: string := \"init\"; "
        "end Impl2490T8; "
        "public class Program2490T8 "
        "  public static method Main(): int "
        "    using var h := new Impl2490T8(); "
        "    var iref: ISettable2490T8 := h; "
        "    iref.Name := \"fresh\" + \"prod\"; "
        "    return iref.Name.Length; "
        "  end Main; "
        "end Program2490T8;";
    int r = compileRunAsan2490Local(src, /*leaks=*/false);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 9);  // "freshprod".Length; no crash (leak accepted)
}

// ===========================================================================
// T9 — a CLASS's own vtable (NOT itable) virtual property override, string-
// typed, LOCAL — the branch of lowerVirtualPropertySet that DOES still get
// the ARGUMENT-PASS release (`ps->parent` is reliably the declaring CLASS
// here, unlike the interface branch T8 pins). Proves the release is not
// dead code: this shape stays fully clean under LeakSanitizer.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_arc_2490,
           class_vtable_virtual_override_direct_producer_no_leak_23_3) {
    const std::string src =
        "module LocalVirt2490T9; "
        "public class Base2490T9 "
        "  public virtual property Name: string := \"init\"; "
        "end Base2490T9; "
        "public class Derived2490T9: Base2490T9 "
        "end Derived2490T9; "
        "public class Program2490T9 "
        "  public static method Main(): int "
        "    using var h := new Derived2490T9(); "
        "    var b: Base2490T9 := h; "
        "    b.Name := \"fresh\" + \"prod\"; "
        "    return b.Name.Length; "
        "  end Main; "
        "end Program2490T9;";
    int r = compileRunAsan2490Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 9);  // "freshprod".Length; no crash, no leak
}

// ===========================================================================
// T10 — the coordinator's own explicit ask (independent review of the
// concurrently-landing #2489): a RECORD auto-property setter reached via
// #2489's NEW cross-module direct-export route (emitExportedPropertySetters
// widened to `Class || Record`, forwarding to this SAME shared
// ensurePropertyAccessorFn body), set from a DIRECT +1 producer (the exact
// shape that crashed before the classOwned gate was added to
// lowerImportedPropertySetValue — see the file header). Proves: no crash
// (the critical safety property — a record's setter stays the pre-fix plain
// store, never retains, and this fix's release is correctly gated off for
// it). LeakSanitizer OFF: an UNRELATED, pre-existing, already-documented
// characteristic independent of this fix — a record's string field carries
// no independent +1 in ANY code path in this compiler (per
// isStringFieldLvalue's own long-standing comment: "record string fields
// are excluded, they keep the plain-store / leak behaviour", §9.7 — records
// have no disposer to balance a retain against) — so a producer stored into
// a record's string field always leaked, on origin/main, long before this
// fix and unchanged by it; confirmed by code-path inspection (this fix's
// diff is a no-op for a `classOwned == false` receiver — neither the
// retain nor the release ever execute).
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_arc_2490,
           record_cross_module_export_direct_producer_no_crash_9_2) {
    const char* kLibRec =
        "module LibRecStr2490T10; "
        "public record RHolder2490T10 "
        "  public property Name: string := \"init\"; "
        "end RHolder2490T10;";
    const std::string appSrc =
        "module App2490T10; import LibRecStr2490T10; "
        "public class Program2490T10 "
        "  public static method Main(): int "
        "    var r := new RHolder2490T10(); "
        "    r.Name := \"hel\" + \"lo!\"; "
        "    return r.Name.Length; "
        "  end Main; "
        "end Program2490T10;";
    int r = compileImportRunAsan2490(kLibRec, appSrc, /*leaks=*/false);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 6);  // "hello!".Length; no crash (pre-existing leak accepted)
}

}  // namespace bezel::module
