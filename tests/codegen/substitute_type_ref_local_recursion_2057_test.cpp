// #2057 (§14 type-resolution + substitution, v0.201) — `substituteTypeRefLocal`
// does NOT recurse into a null-declaration `Named` arg on the failed-head-name
// lookup path, so the args of a null-decl BUILTIN generic (`Array<T>`,
// `Dictionary<K,V>`, `Task<T>`, … — declaration intentionally null per
// §2.5/§29.3) are never substituted when the head name itself is NOT in the
// substitution map.
//
// THE DEFECT. `instanceDescriptorFor` (#1843) walks the super chain under the
// active substitution (`subst`), recording each base's concrete args via
// `substituteTypeRefLocal(bref->genericArgs[gi], subst)`. For a derived class
// `class C<W>: Base<Array<W>>`, the base arg `Array<W>` has declaration=null and
// genericArgs=[W]. When substituting under `{W: int}`:
//   - head = "Array"; map only has "W" → head lookup FAILS
//   - the function returns `Array<W>` UNCHANGED instead of recursing into args
//     to substitute W→int
//   - the recorded base arg is the unsubstituted `Array<W>`, which contains the
//     still-unbound parameter W
//   - `descTypeArgConcrete(W)` returns false → `descTypeArgConcrete(Array<W>)`
//     returns false → `instanceDescriptorFor` conservatively records the
//     SHARED Base descriptor (not a per-monomorphization one)
//   - the runtime descriptor chain of a `C<int>` object contains only the
//     shared Base descriptor, NOT `td$Base$_Array_int` — so a runtime test
//     `o is Base<Array<int>>` walks the chain looking for the per-mono desc,
//     never finds it, and emits FALSE (a sound false-negative).
//
// FIX. Mirror `appendDescKey`'s null-decl-builtin recursion (#2013): on the
// failed-lookup path, recurse into `t->genericArgs` when non-empty (null-decl
// builtin with args). A bare parameter placeholder (null decl + no args) still
// falls through unchanged — it has no arg key to substitute. The substitution
// propagates → args become concrete → per-monomorphization descriptor is minted
// → runtime type test against the actual instantiation is TRUE.
//
// SOUNDNESS. This is a SOUND false-negative fix — the runtime was too
// conservative, declaring `is` FALSE when the correct verdict is TRUE. NO
// false-positive regression: a per-mono `Base<Array<int>>` desc still does not
// match `is Base<Array<string>>` (their keys differ by the substituted int vs
// string arg), so cross-instantiation correctness is preserved.
//
// WHAT THIS TEST PINS. The runtime `is` verdict against the SUBSTITUTED base
// type (the exact reported false-negative). All observed via the process exit
// code. WITHOUT the fix: `is Base<Array<int>>` returns FALSE on a C<int>
// object (false-negative); WITH the fix: returns TRUE.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Shared preamble: a generic `Base<T>` and a derived generic `C<W>` whose base
// arg is a null-decl builtin generic (`Array<W>`) holding the derived's
// parameter W. Instantiating `C<int>` triggers the substitution path under
// `{W: int}` and walks Base's args through `substituteTypeRefLocal`.
const char* kBaseCArray =
    "module M; "
    "class Base<T> public tag: int; "
    "  public constructor(t: int) tag := t; end constructor; "
    "end Base; "
    "class C<W>: Base<Array<W>> "
    "  public constructor() base(99); end constructor; "
    "end C; ";

// ---- THE HEADLINE: substituted base arg is distinguishable ----------------

// `o is Base<Array<int>>` on a `C<int>` instance. Pre-fix the per-mono
// descriptor for `Base<Array<int>>` is NOT minted (the substituted args stay
// non-concrete because W→int propagation is lost), so the descriptor chain
// carries only the shared Base descriptor, and the per-mono target never
// matches → FALSE. Post-fix the per-mono desc IS minted, chain contains it,
// target matches → TRUE.
BEZEL_TEST(codegen_2057, substituted_base_arg_matches) {
    BEZEL_EXPECT_EQ(run(
        std::string(kBaseCArray) +
        "class MainClass public static method Main(): int "
        "  using var c := new C<int>(); "
        "  var o: Object := c; "
        "  return (1 if o is Base<Array<int>> else 0); "
        "end Main; end MainClass;"), 1);
}

// ---- CONTROL: the matching arg is the one the runtime looks for -----------

// `o is Base<Array<string>>` on a `C<int>` instance MUST still be FALSE — the
// per-mono descriptor for `Base<Array<int>>` (post-fix) differs from
// `Base<Array<string>>`'s by the substituted arg's identity, so the chain walk
// does not match a mismatching target. Pins that the fix does NOT collapse
// `Array<int>` and `Array<string>` into a single shared key (a sanity guard
// against an over-broad recursion that lost the arg distinction).
BEZEL_TEST(codegen_2057, mismatching_base_arg_still_false) {
    BEZEL_EXPECT_EQ(run(
        std::string(kBaseCArray) +
        "class MainClass public static method Main(): int "
        "  using var c := new C<int>(); "
        "  var o: Object := c; "
        "  return (1 if o is Base<Array<string>> else 0); "
        "end Main; end MainClass;"), 0);
}

// ---- THE HEADLINE two-arm: combined is / as in one program ----------------

// Combined correctness on one process exit code: r1 = `is Base<Array<int>>`
// (true positive on the actual instantiation, post-fix), r2 =
// `is Base<Array<string>>` (false positive avoidance), r3 = `as
// Base<Array<int>>` non-null. Expected = 1*100 + 0*10 + 1 = 101. Pre-fix
// returned 1 (only r3 succeeded — `as` to ANY generic base on a derived class
// conservatively collapses through the shared descriptor, while the
// runtime-target `is`/`as` lookup for a per-mono target could not resolve, so
// r1 and r2 were both FALSE; the shared-desc path on `as` resolves to a
// non-null typed binding because the object IS-A Base via the shared desc).
// Re-pinning with r1=1 is the false-negative the fix corrects.
BEZEL_TEST(codegen_2057, combined_is_as_substituted_base) {
    BEZEL_EXPECT_EQ(run(
        std::string(kBaseCArray) +
        "class MainClass public static method Main(): int "
        "  using var c := new C<int>(); "
        "  var o: Object := c; "
        "  var r1 := (1 if o is Base<Array<int>> else 0); "
        "  var r2 := (1 if o is Base<Array<string>> else 0); "
        "  var x := o as Base<Array<int>>; "
        "  var r3 := (1 if x <> null else 0); "
        "  return r1 * 100 + r2 * 10 + r3; "
        "end Main; end MainClass;"), 101);
}

// ---- DEEPER DEPTH: nested null-decl builtin in the substituted arg --------

// `class C<W>: Base<Array<Array<W>>>` — the substitution must propagate TWO
// levels into the nested null-decl builtin arg. Pre-fix the inner W→int is
// lost (the failed head-name path returns `Array<Array<W>>` unchanged), so
// both `W`s stay non-concrete → shared descriptor → false-negative on
// `is Base<Array<Array<int>>>`. Post-fix the recursion reaches the inner W
// through the failed-lookup path and produces `Array<Array<int>>`, all args
// concrete → per-mono desc minted → TRUE.
BEZEL_TEST(codegen_2057, nested_null_decl_builtin_in_base_arg) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base<T> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; "
        "end Base; "
        "class C<W>: Base<Array<Array<W>>> "
        "  public constructor() base(99); end constructor; "
        "end C; "
        "class MainClass public static method Main(): int "
        "  using var c := new C<int>(); "
        "  var o: Object := c; "
        "  return (1 if o is Base<Array<Array<int>>> else 0); "
        "end Main; end MainClass;"), 1);
}

// ---- CONTROL: a non-builtin (declared) generic base arg is unaffected -----

// `class C<W>: Base<W>` — the base arg `W` is a bare parameter placeholder (a
// type-variable reference, declaration=null and genericArgs empty). The
// placeholder branch (the original intent) still resolves W→int via the
// head-name map. Pre-fix and post-fix both work; pins that the fix does not
// regress the bare-placeholder path.
BEZEL_TEST(codegen_2057, control_bare_placeholder_substitution_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base<T> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; "
        "end Base; "
        "class C<W>: Base<W> "
        "  public constructor() base(99); end constructor; "
        "end C; "
        "class MainClass public static method Main(): int "
        "  using var c := new C<int>(); "
        "  var o: Object := c; "
        "  return (1 if o is Base<int> else 0); "
        "end Main; end MainClass;"), 1);
}

// ---- CONTROL: reified declared-arg generic base keeps working ------------

// `class C<W>: Base<List<W>>` — `List` is a user-declared class (NOT a
// null-decl builtin), so the head-declaration branch in
// `substituteTypeRefLocal` already substitutes inside the args. The fix must
// not disturb this path. The `is Base<List<int>>` test exercises both
// directions: matching (TRUE) and mismatching arg (FALSE) for the same
// runtime-typed substitution.
BEZEL_TEST(codegen_2057, control_declared_generic_base_arg_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class List<T> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; "
        "end List; "
        "class Base<T> public tag: int; "
        "  public constructor(t: int) tag := t; end constructor; "
        "end Base; "
        "class C<W>: Base<List<W>> "
        "  public constructor() base(99); end constructor; "
        "end C; "
        "class MainClass public static method Main(): int "
        "  using var c := new C<int>(); "
        "  var o: Object := c; "
        "  var r1 := (1 if o is Base<List<int>> else 0); "
        "  var r2 := (1 if o is Base<List<string>> else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end MainClass;"), 10);
}

}  // namespace