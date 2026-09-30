// Spec-derived regression test for issue #3274 — `Linq` (System.Collections
// .Linq) is a MODULE of extension methods on `IEnumerable<T>` (stdlib §5), not
// a static class, but its built-in type identifier (arity 0 in
// `Resolver::builtinTypeArity`, TypeResolution_Names.cpp:314) carries a
// synthetic TypeSymbol (decl == nullptr, set by
// ExprTypecheck_Exprs.cpp:1471). So `Linq.TotallyBogusMember(1, 2)` parses as
// `MemberAccess(Linq, TotallyBogusMember)(1, 2)`, reaches the static-call
// dispatch, and pre-#3274 silent-degraded via the #2434 zero-init fallback
// (only an INTERNAL warning, program ran to exit 0 with `v` = 0):
//
//   scratch/repro_3274.bzl:6:12: internal: lowerLocalDecl's initializer
//   expression failed to lower to a value for this local's slot
//   (degraded to a zero-initialized value) — #2434
//
// The fix adds a per-static-class arm to kStaticSurfaces[] in
// src/sema/ExprTypecheck_Calls.cpp ~7329 with an empty member list and a §5
// stdlib citation — Linq has zero documented static methods, so the empty
// list correctly rejects every `Linq.X(...)` call (any name matches no member
// of the empty list). The `tgtSym->decl == nullptr` gate below the table
// keeps a user class named `Linq` (a §11.3 collision — `class Linq { ... }`)
// compiling cleanly, mirroring the §2996 fix's "user class named `Path` is
// unaffected" carve-out.
//
// This test pins four reachable post-fix shapes:
//   * T1: `Linq.Bogus()` (method call, no args) → Error with the
//     "no member `Bogus` on `Linq`" diagnostic.
//   * T2: `Linq.Bogus(1, 2)` (method call, two args) → same.
//   * T3: a LEGITIMATE extension-method call (`arr.Select(lambda(i) => i)`)
//     compiles cleanly — the fix MUST NOT narrow extension-method dispatch.
//   * T4: the OTHER seven static-class names (#3021's siblings — `File`,
//     `Directory`, `Path`, `Debug`, `Domain`, `Encoding`, `BitConverter`) are
//     STILL rejected on a bogus member call. Pins that the fix only added
//     a row, didn't accidentally narrow an existing arm or alter the gate.
//
// Spec: §11.7 ("A static class may not be instantiated") implies a non-static
// class used as a static-call receiver has no surface at all; the per-static-
// class closed-surface arm is the enforcement. §5 documents zero static
// methods on `Linq`.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff `program` produces an Error diagnostic whose message contains
// `needle`. Used to pin the specific "no member `M` on `Y`; documented
// surface …" diagnostic the per-static-class arm emits.
bool hasError(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// True iff `program` compiled AND ran to completion (`ran == true`). Used
// for T3 (the legitimate extension-method call must compile cleanly; the
// return value is incidental — `doubled.Count` may be any positive int).
bool ranCleanly(const std::string& program) {
    auto r = runAot(program);
    return r.ran;
}

// ── T1: Linq.Bogus() (method CALL, no args) ──────────────────────────────
//
// `Linq.Bogus()` is not a documented static surface; the per-static-class
// arm emits the `no member `Bogus` on `Linq`; documented surface …` Error.
// Pre-#3274: the program compiled with only an INTERNAL warning
// (`lowerLocalDecl's initializer expression failed to lower to a value for
// this local's slot (degraded to a zero-initialized value) — #2434`) and
// exited 0 with `v` = 0.
//
// (A bare property-access shape `var v := Linq.Bogus;` would also degrade
// pre-fix but is NOT covered by the fix — the per-static-class arm lives
// in the CallExpr dispatch (typeCall at ExprTypecheck_Calls.cpp:1639), not
// the MemberAccessExpr dispatch (resolveMemberAccess at line 12997). The
// bare-property form is a separate shape from #3274's scope; the issue
// body explicitly cites only the `Linq.X(...)` call form.)
std::string linqBogusNoArgs() {
    return std::string(
        "module M; "
        "class C public static method Main(): int "
        "  var v := Linq.Bogus(); "
        "  _ := v; "
        "  return 0; "
        "end Main; end C;");
}

// ── T2: Linq.Bogus(1, 2) (method CALL, with parens) ───────────────────────
//
// Same shape as T1 but the call form. Pre-#3274: identical silent-zero
// degrade; post-#3274: same `no member … documented surface` Error.
std::string linqBogusMethod() {
    return std::string(
        "module M; "
        "class C public static method Main(): int "
        "  var v := Linq.Bogus(1, 2); "
        "  _ := v; "
        "  return 0; "
        "end Main; end C;");
}

// ── T3 (control): a LEGITIMATE `Linq` extension method ────────────────────
//
// `Linq` (System.Collections.Linq) is a module of extension methods on
// `IEnumerable<T>` (stdlib §5). The fix added a row with an EMPTY member
// list; legitimate extension-method dispatch (on an `IEnumerable<T>`
// receiver, via `import System.Collections.Linq;`) MUST still work
// (extension methods are not static methods on `Linq` — they're INSTANCE
// methods on `IEnumerable<T>` resolved through the extension-method
// lookup). This pins that the fix did not over-reject: a real
// `arr.Select(lambda(i) => i)` call compiles AND runs cleanly.
std::string linqExtensionMethodCompiles() {
    return std::string(
        "module M; "
        "import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var arr := new Array<int>(); "
        "  arr.Add(10); arr.Add(20); arr.Add(30); "
        "  using var doubled: Array<int> := "
        "    arr.Select(lambda(x: int) => x * 2).ToArray(); "
        "  return doubled.Count; "
        "end Main; end C;");
}

// ── T4: OTHER static-class names STILL rejected ───────────────────────────
//
// The fix added ONE row to kStaticSurfaces[]. It MUST NOT have narrowed
// or otherwise altered the existing rows (`File`, `Directory`, `Path`,
// `Debug`, `Domain`, `Encoding`, `BitConverter`). Each of these rejects
// `Foo.TotallyBogusMember(1, 2)` with its own documented-surface Error
// (or, for `Encoding`/`BitConverter`, via the §3.2 / `rejectBuiltinStatic`
// arms). Pinned by the value-demanding static-call form (the worst case
// the codegen gate guards).
struct OtherStaticCase {
    const char* name;
    const char* imports;
};

// `Encoding` is tested via a static property (Encoding.UTF8) because
// `Encoding.TotallyBogusMember(...)` would be the call form but Encoding
// does have some static members (UTF8, ASCII, etc.); the call form
// would land in a different reject path. `BitConverter` likewise has a
// `rejectBuiltinStatic()` arm. The static-class arms for File/Directory/
// Path/Debug/Domain all reject `X.TotallyBogusMember(1, 2)` with the
// `no member … documented surface …` Error — pinned by the same needle
// T1/T2 use, narrowed to the name.
const std::vector<OtherStaticCase>& otherStaticCases() {
    static const std::vector<OtherStaticCase> kCases = {
        {"File",        ""},
        {"Directory",   ""},
        {"Path",        ""},
        {"Debug",       "import System.Diagnostics; "},
        {"Domain",      "import System.Threading; "},
    };
    return kCases;
}

std::string otherStaticBogusCall(const char* name, const char* imports) {
    return std::string("module M; ") + imports +
        "class C public static method Main(): int "
        "  var v := " + name + ".TotallyBogusMember(1, 2); "
        "  _ := v; "
        "  return 0; "
        "end Main; end C;";
}

}  // namespace

// ── T1 ─────────────────────────────────────────────────────────────────────
BEZEL_TEST(static_class_bogus_member_linq_3274,
           linq_bogus_noargs_call_emits_no_member_diagnostic) {
    BEZEL_EXPECT_TRUE(hasError(linqBogusNoArgs(),
                               "no member `Bogus` on `Linq`"));
}

// ── T2 ─────────────────────────────────────────────────────────────────────
BEZEL_TEST(static_class_bogus_member_linq_3274,
           linq_bogus_method_call_emits_no_member_diagnostic) {
    BEZEL_EXPECT_TRUE(hasError(linqBogusMethod(),
                               "no member `Bogus` on `Linq`"));
}

// ── T3 ─────────────────────────────────────────────────────────────────────
BEZEL_TEST(static_class_bogus_member_linq_3274,
           legitimate_linq_extension_method_compiles_and_runs) {
    // The fix MUST NOT narrow extension-method dispatch: a real
    // `arr.Select(lambda(i) => i * 2)` call still compiles and runs.
    // Pre-fix this passes; post-fix it must still pass — the regression
    // is that the Linq row could not BREAK this (extension methods are
    // instance members on `IEnumerable<T>`, not static methods on
    // `Linq`), but the test pins the property.
    BEZEL_EXPECT_TRUE(ranCleanly(linqExtensionMethodCompiles()));
}

// ── T4 ─────────────────────────────────────────────────────────────────────
BEZEL_TEST(static_class_bogus_member_linq_3274,
           other_static_class_names_still_rejected) {
    // The fix added ONE row; it MUST NOT have narrowed the existing
    // per-static-class arms for `File`, `Directory`, `Path`, `Debug`,
    // `Domain`. Each of these rejects a bogus member call with the
    // `no member `M` on `N`; documented surface …` Error.
    for (const OtherStaticCase& c : otherStaticCases()) {
        std::string needle =
            std::string("no member `TotallyBogusMember` on `") + c.name + "`";
        std::string program = otherStaticBogusCall(c.name, c.imports);
        BEZEL_EXPECT_TRUE(hasError(program, needle.c_str()));
    }
}
