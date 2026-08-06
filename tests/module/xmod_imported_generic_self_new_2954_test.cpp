// Spec-derived regression test for issue #2954.
//
// §22.8 (Field initializers) / §2.8.3 rule 4 (cross-artifact type identity) /
// §14.1 (Generic declarations) / §2.5 / §12, v0.240.
//
// THE DEFECT. `new Self<T>()` evaluated INSIDE an imported generic class's own
// re-lowered method body produced an object whose field initializers never ran.
// The same `new Self<Arg>()` written in the importing module was correct, so the
// two spellings of one construction disagreed — §2.8.3 rule 4 draws no such
// distinction ("regardless of which artifact produced the value").
//
// THE ISSUE'S HYPOTHESIS IS NOT WHERE THE FIX LIVES, and the difference matters
// because it is a whole category wider. #2954 suspected "the constructor /
// field-initializer sequence ... is reconstructed only on the importing module's
// own `new` path". It is not: the inner `new` never reaches an imported class at
// all. `ModuleLoader::buildSyntheticMethodSource` (and its three siblings)
// re-parses the body against a SYNTHETIC mini-class that redeclares every field
// as `name : Type;` — WITHOUT its initializer — and the body's `new Self<T>()`
// resolves to THAT class. It is a different type from the imported one in every
// way codegen cares about:
//
//   * no field initializers, so §22.8's "Field initializers run as part of
//     construction in textual declaration order" had nothing to run;
//   * `ctx_.isImported()` is false for it, so `lowerNew` took neither the
//     imported-generic branch nor the local one and NO VTABLE was installed;
//   * `classStruct` minted it a SECOND `%class.<Name>$spec0`, so one
//     monomorphization had two struct types and two identities.
//
// MEASURED on this fix's parent, all arms:
//
//     nested=21        the issue's repro; 42 is correct (inner 21 + outer 21)
//     str=<empty>      then SIGSEGV — dispatching on the inner object read a
//                      NULL vtable word, so this arm is a hard fault, not a
//                      wrong value
//     wide=            SIGSEGV — `inner.note` is a null string
//     nested_int=21    at `T = int`, so the defect is NOT layout-gated
//     arity2=21        at `<A, B>`
//
// THE FIX rebinds every TypeRef the re-parsed body resolved to the synthetic
// self-class so it names the IMPORTED TypeSymbol, in all four reconstruction
// paths (method / constructor / destructor / field-initializer bodies). #2954
// requires this direction in as many words — "whatever runs the initializers has
// to be the SAME sequence the importing module's own `new GRe10<string>()`
// runs, not a second reconstruction of it". Re-emitting the initializers into
// the synthetic source would have been that second reconstruction, and would
// have left the type identity split.
//
// NOT FIXED HERE, measured not assumed: an `is` test against an imported generic
// INSTANTIATION answers false — `td$<Name>_<args>` at the test site vs
// `td$<Name>$specN` on the object. That reproduces with no re-lowered body
// involved (`g is GRe<string>` in the importing module's own `Main` is already
// false on this fix's parent AND with it), so it is a separate, pre-existing
// defect and is filed separately. No case here asserts on an `is` test.
//
// Built the same way as its siblings tests/module/xmod_imported_generic_layout_
// moving_2943_test.cpp and xmod_imported_generic_object_slots_2953_test.cpp: a
// real `bezelc -c` + `--import` + a native run. Values are kept < 256 because
// the observable is the exit status, and NEVER 0 — this defect's whole signature
// is a field silently reading 0.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2954 {
    std::string path;
    explicit TempFile2954(std::string p) : path(std::move(p)) {}
    ~TempFile2954() { std::remove(path.c_str()); }
};

void writeSource2954(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2954(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link OR did not exit normally (a SIGSEGV lands here too — which is
// what T2 and T3 observed on this fix's parent).
int compileImportAndRun2954(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2954_" + std::to_string(std::rand());
    TempFile2954 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2954 aObj((dir / (stem + "_a.o")).string());
    writeSource2954(aSrc.path, libSrc);
    if (!compileToObj2954(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2954 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2954 bExe((dir / stem).string());
    writeSource2954(bSrc.path, appSrc);
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

}  // namespace

// ⚠ MAINTENANCE NOTE — THE PREMISES, AND HOW EACH ONE EXPIRES.
//
// (1) THE `new` IS INSIDE THE IMPORTED CLASS'S OWN BODY and names the class
// itself. That is the whole defect: a `new` naming any OTHER type is a
// #710/#2290 sibling-type reference the reconstruction declines outright, and a
// `new` written in the IMPORTING module is the correct path this fix makes the
// inner one share. Do NOT "simplify" a case by moving its `new` out of the
// library body — the case becomes vacuous.
//
// (2) THE INITIALIZED FIELD IS NON-ZERO. `_w: int := 21` is what separates "the
// initializer ran" from "calloc zeroed it". A case rewritten to `:= 0` would
// pass with AND without the fix.
//
// (3) THE LAYOUT PREMISE IS *NOT* REQUIRED HERE, and that is a MEASURED result,
// not an oversight: T4 pins `T = int` — where the composed and erased layouts
// coincide — and it FAILED without the fix exactly as `T = string` did. The two
// `T`-typed fields the sibling files (#2894/#2928/#2943/#2947/#2953) require are
// kept for shape consistency, but no case here depends on them. Removing them
// would not make these cases vacuous; removing premise (1) or (2) would.

// ===========================================================================
// T0 — THE PREMISE CANARY. The OUTER construction — the importing module's own
// `new` — must run the field initializer, i.e. `g._w` is 21. That arm was always
// correct, and it is what makes the inner arm's 0 a DISAGREEMENT between two
// spellings of one construction rather than a uniform failure. §22.8 / §2.5.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_self_new_2954, outer_construction_canary_spec_22_8) {
    const char* lib =
        "module GSelfLib2954T0; "
        "public class GSelf2954T0<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public method Nested(): int "
        "    using var inner := new GSelf2954T0<T>(); "
        "    return inner._w + _w; "
        "  end Nested; "
        "end GSelf2954T0;";
    int r = compileImportAndRun2954(
        lib,
        "module GSelfApp2954T0; import GSelfLib2954T0; "
        "public class M2954T0 "
        "  public static method Main(): int "
        "    using var g := new GSelf2954T0<string>(); "
        "    if g._w <> 21 then return 91; end; "
        "    return 44; "
        "  end Main; "
        "end M2954T0;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T1 — THE ISSUE'S OWN REPRO. `inner._w + _w`: both operands are 21, so 42 is
// correct and 21 is exactly "the inner object's initializer did not run".
// Measured 21 on this fix's parent. §22.8 / §2.8.3 rule 4.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_self_new_2954, self_new_runs_field_initializers_spec_22_8) {
    const char* lib =
        "module GSelfLib2954T1; "
        "public class GSelf2954T1<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public method Nested(): int "
        "    using var inner := new GSelf2954T1<T>(); "
        "    return inner._w + _w; "
        "  end Nested; "
        "end GSelf2954T1;";
    int r = compileImportAndRun2954(
        lib,
        "module GSelfApp2954T1; import GSelfLib2954T1; "
        "public class M2954T1 "
        "  public static method Main(): int "
        "    using var g := new GSelf2954T1<string>(); "
        "    return g.Nested(); "
        "  end Main; "
        "end M2954T1;");
    if (r == -2) return;
    // 21 (inner, from the initializer) + 21 (outer `this`).
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T2 — THE VTABLE ARM, which the issue does not mention and which is a HARD
// FAULT rather than a wrong value: the inner object's word 0 was never written,
// so dispatching anything on it dereferenced a null vtable. Measured on this
// fix's parent as an empty `ToString()` result followed by SIGSEGV.
//
// The receiver is `Object`-typed so the call genuinely goes through the vtable
// (§8.5 "via virtual dispatch"), and the assertion is on the EXACT string, so a
// vtable that is merely non-null but wrong still fails. §12 / §8.5.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_self_new_2954, self_new_installs_vtable_spec_12) {
    const char* lib =
        "module GSelfLib2954T2; "
        "public class GSelf2954T2<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public override method ToString(): string "
        "    return \"w=\" + _w.ToString(); "
        "  end ToString; "
        "  public method NestedStr(): string "
        "    using var inner := new GSelf2954T2<T>(); "
        "    var o: Object := inner; "
        "    return o.ToString(); "
        "  end NestedStr; "
        "end GSelf2954T2;";
    int r = compileImportAndRun2954(
        lib,
        "module GSelfApp2954T2; import GSelfLib2954T2; "
        "public class M2954T2 "
        "  public static method Main(): int "
        "    using var g := new GSelf2954T2<string>(); "
        "    var s: string := g.NestedStr(); "
        "    if s <> \"w=21\" then return 91; end; "
        "    return 44; "
        "  end Main; "
        "end M2954T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T3 — THE WHOLE STORAGE SEQUENCE, not one `int`. `_z` has NO initializer and
// must stay 0; `_w` has one and must be 21; `note` has a STRING one and must be
// a live string — pre-fix it was null, so `inner.note.Length` was a null
// dereference and this case SIGSEGV'd. The asserted value is composed from all
// three (0 + 21 + 4 + 96), so a fix that ran only the integer initializers, or
// only the first one, still fails. §22.8 "in textual declaration order" / §4.6.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_self_new_2954, self_new_runs_whole_initializer_sequence_spec_22_8) {
    const char* lib =
        "module GSelfLib2954T3; "
        "public class GSelf2954T3<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _z: int; "
        "  public _w: int := 21; "
        "  public note: string := \"keep\"; "
        "  public method NestedWide(): int "
        "    using var inner := new GSelf2954T3<T>(); "
        "    return inner._z + inner._w + inner.note.Length + 96; "
        "  end NestedWide; "
        "end GSelf2954T3;";
    int r = compileImportAndRun2954(
        lib,
        "module GSelfApp2954T3; import GSelfLib2954T3; "
        "public class M2954T3 "
        "  public static method Main(): int "
        "    using var g := new GSelf2954T3<string>(); "
        "    return g.NestedWide(); "
        "  end Main; "
        "end M2954T3;");
    if (r == -2) return;
    // 0 (no initializer) + 21 + 4 ("keep") + 96.
    BEZEL_EXPECT_EQ(r, 121);
}

// ===========================================================================
// T4 — `T = int`, THE LAYOUT-IDENTICAL INSTANTIATION, AND IT IS NOT A CONTROL.
// This is the measured evidence that #2954 is NOT a member of the
// placeholder-LAYOUT family (#2019 / #2894 / #2928 / #2943 / #2947 / #2953): at
// `T = int` the composed and erased layouts coincide, and this case still
// returned 21 on the fix's parent. The synthetic self-class has no initializers
// whatever the substitution is. §14.1 / §22.8.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_self_new_2954, defect_is_not_layout_gated_spec_14_1) {
    const char* lib =
        "module GSelfLib2954T4; "
        "public class GSelf2954T4<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public method Nested(): int "
        "    using var inner := new GSelf2954T4<T>(); "
        "    return inner._w + _w; "
        "  end Nested; "
        "end GSelf2954T4;";
    int r = compileImportAndRun2954(
        lib,
        "module GSelfApp2954T4; import GSelfLib2954T4; "
        "public class M2954T4 "
        "  public static method Main(): int "
        "    using var g := new GSelf2954T4<int>(); "
        "    return g.Nested(); "
        "  end Main; "
        "end M2954T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T5 — GENERIC ARITY 2. The rebind is over the whole TypeRef graph and is keyed
// on the declaration, not on a parameter count; `new Self<A, B>()` inside the
// body is the same shape. Measured 21 on the fix's parent. §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_self_new_2954, arity_two_self_new_spec_14_1) {
    const char* lib =
        "module GSelfLib2954T5; "
        "public class GSelf2954T5<A, B> "
        "  public _a: A; "
        "  public _b: B; "
        "  public _w: int := 21; "
        "  public method Nested(): int "
        "    using var inner := new GSelf2954T5<A, B>(); "
        "    return inner._w + _w; "
        "  end Nested; "
        "end GSelf2954T5;";
    int r = compileImportAndRun2954(
        lib,
        "module GSelfApp2954T5; import GSelfLib2954T5; "
        "public class M2954T5 "
        "  public static method Main(): int "
        "    using var g := new GSelf2954T5<string, string>(); "
        "    return g.Nested(); "
        "  end Main; "
        "end M2954T5;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T6 — DECLARED CONTROL: THE SAME-MODULE SPELLING. A generic class declared in
// the CONSUMING module, doing exactly what T1's library class does, was correct
// throughout — there is no re-parse and no synthetic self-class, so `new
// Self<T>()` names the real declaration. This is the reference behaviour
// §2.8.2 clause 2 requires the imported spelling to match, and it passes with
// AND without the fix. The class here is local to the app; the library is
// imported and used so the two-module build is genuine. §2.5 / §22.8.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_self_new_2954, same_module_spelling_control_spec_22_8) {
    const char* lib =
        "module GSelfLib2954T6; "
        "public class Tag2954T6 "
        "  public _k: int := 0; "
        "end Tag2954T6;";
    int r = compileImportAndRun2954(
        lib,
        "module GSelfApp2954T6; import GSelfLib2954T6; "
        "class GLocal2954T6<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public method Nested(): int "
        "    using var inner := new GLocal2954T6<T>(); "
        "    return inner._w + _w; "
        "  end Nested; "
        "end GLocal2954T6; "
        "public class M2954T6 "
        "  public static method Main(): int "
        "    using var t := new Tag2954T6(); "
        "    if t._k <> 0 then return 91; end; "
        "    using var g := new GLocal2954T6<string>(); "
        "    return g.Nested(); "
        "  end Main; "
        "end M2954T6;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T7 — RE-ENTRANCY AND ONE IDENTITY, the case this fix makes REACHABLE for the
// first time. `Equals` — one of the three FIXED Object slots — constructs its
// own instantiation, and #2953 emits that slot's specialized body from inside
// `buildImportedGenericVtable`. Before this fix the inner `new` resolved to the
// synthetic self-class and never entered that function at all; now it does, so
// the function re-enters itself for the same instantiation while mid-emission.
//
// #2943 made that safe by registering the vtable global BEFORE any body
// emission; this case is what exercises it. The property asserted is §2.8.3
// rule 4's — "one runtime type identity per monomorphization ... regardless of
// which artifact produced the value": verified in the emitted IR as exactly one
// `vt$<Name>$spec0` global, stored by BOTH the importing module's own `new` and
// the one inside the re-lowered body.
//
// `inner._w + _w = 42` is the observable: 42 only if the INNER object's
// initializer ran, and the `Equals` dispatch only reaches the right body if
// #2953's slot 4 specialization is in place — so this case needs both fixes and
// is pinned in this file because it is the one that lands second. §2.8.3 rule 4
// / §8.3 / §22.8.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_self_new_2954, reentrant_object_slot_one_identity_spec_2_8_3) {
    const char* lib =
        "module GSelfLib2954T7; "
        "public class GSelf2954T7<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public override method Equals(other: Object?): bool "
        "    using var inner := new GSelf2954T7<T>(); "
        "    return inner._w + _w = 42; "
        "  end Equals; "
        "  public override method GetHashCode(): int "
        "    return _w + 23; "
        "  end GetHashCode; "
        "end GSelf2954T7;";
    int r = compileImportAndRun2954(
        lib,
        "module GSelfApp2954T7; import GSelfLib2954T7; "
        "public class M2954T7 "
        "  public static method Main(): int "
        "    using var g := new GSelf2954T7<string>(); "
        "    var o: Object := g; "
        "    if not o.Equals(o) then return 91; end; "
        "    return o.GetHashCode(); "
        "  end Main; "
        "end M2954T7;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

}  // namespace bezel::module
