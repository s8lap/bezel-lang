// fix #1276 / §26.4/§26.5: an extension declared on a USER-DECLARED generic
// type (e.g. `extension IBox<T>`) WITHOUT a `where` clause was never matched
// against any receiver, because sema's lookupExtensionMember only scanned an
// extension target's generic-arg positions for implicit free type variables
// (§26.4: "each bare single-segment non-nullable identifier introduces a
// fresh block-level type parameter — always") when the target's outer type
// was a BUILTIN (null-declaration, e.g. `IEnumerable<T>`). TypeResolution
// itself applies the §26.4 binding unconditionally — a user-declared target
// like `IBox<T>` resolves `T` to the exact same null-declaration placeholder
// ref as a builtin target does (TypeResolution_Names.cpp resolveTopDecl) —
// so gating the *matcher's* free-var scan to builtin targets left every
// plain (no-`where`) user-declared generic extension unmatched.
//
// Symptom differed by receiver kind because of a second, independent gap:
// sema's "no member" diagnostic (ExprTypecheck_Calls.cpp, issue #414) is
// deliberately permissive for INTERFACE-typed receivers (their member set is
// not "known complete"). So:
//   - a CLASS receiver (`b.Const88()` on `b: C`) hit a real "no member"
//     compile error (FALSE_REJECT), while
//   - an INTERFACE-typed receiver (`x.Const88()` on `x: IBox<int>`) compiled
//     with no error and no resolved MethodSymbol/ExtBinding at all — codegen
//     then lowered the call through no dispatch path, producing a garbage
//     return and skipping the extension body entirely (WRONG_VALUE
//     miscompile, the original report).
//
// The fix broadens the free-var scan in lookupExtensionMember
// (src/sema/ExprTypecheck.cpp) to run for ANY Named extension target
// (dropping the `!target->declaration` builtin-only gate), matching
// TypeResolution's own unconditional §26.4 binding. Both shapes below now
// resolve and dispatch correctly.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// `IBox<T>` (a user-declared generic interface) + a `C: IBox<int>` + a
// no-`where`-clause `extension IBox<T>` declaring `body`.
std::string prog(const std::string& body) {
    return std::string(
        "module M; "
        "interface IBox<T> "
        "  method Get(): T; "
        "end IBox; "
        "extension IBox<T> "
        "  public method Const88(): int => 88; "
        "end; "
        "class C: IBox<int> "
        "  public method Get(): int => 5; "
        "end C; "
        "class App public static method Main(): int ") +
        body + " end Main; end App;";
}
}  // namespace

// §26.4/§26.5 spec_1276_1 (the repro) — a generic-interface extension method
// called through an INTERFACE-TYPED receiver (`x: IBox<int> := b;
// x.Const88()`). Was: garbage (body never ran). Should be: 88.
BEZEL_TEST(generic_interface_extension_1276, interface_typed_receiver_dispatches) {
    BEZEL_EXPECT_EQ(runFull(prog(
        "using var b := new C(); "
        "var x: IBox<int> := b; "
        "var v := x.Const88(); "
        "return v; ")), 88);
}

// §26.4/§26.5 spec_1276_2 — the same generic-interface extension method
// called directly on the implementing CLASS receiver (`b.Const88()`, no
// intermediate interface-typed variable). Was: compile error ("no member
// `Const88` on type `C`", FALSE_REJECT). Should be: 88.
BEZEL_TEST(generic_interface_extension_1276, class_receiver_dispatches) {
    BEZEL_EXPECT_EQ(runFull(prog(
        "using var b := new C(); "
        "var v := b.Const88(); "
        "return v; ")), 88);
}

// §26.4 spec_1276_3 — a bare single-segment identifier in a USER-DECLARED
// extension target's generic-arg position (`extension Box<W>`, no `where`
// clause) is a fresh, shadowing block-level type parameter per §26.4
// ("always, shadowing any same-named type in scope") EVEN WHEN a same-named
// class `W` is in scope — it is never a reference to that class. It
// therefore applies to `Box<A>` just as `extension Box<T>` would. Before
// #1276's fix this was a FALSE_REJECT ("no member `Tag`") because
// lookupExtensionMember's free-var scan was gated to builtin (null-decl)
// targets only. Mirrors the corrected
// generic_extension_773_test.cpp::concrete_generic_extension_not_misapplied
// (which now uses a genuinely concrete `Box<int>` for the negative case).
BEZEL_TEST(generic_interface_extension_1276, bare_identifier_shadows_and_applies) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box<T> public _v: T; "
        "  public constructor(v: T) _v := v; end; end Box; "
        "class W public v: int; public constructor(n: int) v := n; end; end W; "
        "class A public n: int := 0; end A; "
        "public extension Box<W> "
        "  public method Tag(): int => 7; "
        "end; "
        "public static class P public static method Main(): int "
        "  using var b := new Box<A>(new A()); "
        "  return b.Tag(); "
        "end Main; end P;"), 7);
}
