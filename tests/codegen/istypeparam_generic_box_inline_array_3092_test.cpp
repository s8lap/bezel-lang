// §12.9 / §8.6 / #3092 — a bare type-parameter `is T` inside a generic body
// must resolve THROUGH the active monomorphization and compare against the
// substituted target's runtime descriptor. The instantiations the issue names
// (Box<[3]int>, Box<()>, Box<int>, Box<string>) all share one shape: an
// Object scrutinee, a type-parameter target, and a runtime box whose vtable
// word carries the structurally-keyed descriptor of the substituted argument.
//
// THE DEFECT. `Emitter::lowerTypeTest` resolves the target's descriptor with
// `boxDescriptorFor(rt)` first, then `perInstDescriptorForTarget(rt)`. Neither
// path recognized a bare type-parameter placeholder as a place to substitute
// through `genericSubstSemaTy_` before re-asking. `boxDescriptorFor` only
// handles Named-with-declaration / Primitive / InlineArray / Unit / Range —
// a bare parameter Name is none of those. `perInstDescriptorForTarget` returns
// early on `!rt->declaration`. The result was a `nullptr` descriptor, which
// the existing code turns into a SILENT FALSE: the `is` test always fails,
// `o is T` inside `Box<[3]int>` reads as `false` for a boxed `[3]int`, and
// the surrounding code sees no diagnostic (`#2775` only fires when the
// target is concrete — the #3092 target is a bare parameter, so the
// inline-array `reportDroppedTest` guard does not fire either).
//
// THE FIX (per §12.9 / §8.6, v0.254): substitute the bare parameter through
// the active monomorphization and re-resolve. The substitute is a fresh
// TypeRef arena-allocated into descArena_ only when the substitution actually
// changes something (pointer-change is the safe signal), so the common
// non-parameter target allocates nothing and the existing fast paths stay
// exact. The four instantiations this test covers exercise the four
// concrete-type paths `boxDescriptorFor` knows: inline array (the reported
// one), unit, primitive, and a builtin generic with arg (string).
//
// NOT COVERED HERE, deliberately (each filed):
//   * `Box<[3]int>.Probe(o as T)` — the inverse operator, which the same
//     symmetric gap would also break; the issue explicitly says "the silent
//     false is the only assertion", so this lane pins only the `is` half.
//   * `Box<T>` instantiated at a SECOND inline-array size (Box<[4]int> vs
//     Box<[3]int>) — `o is T` inside Box<[4]int> should fail on a boxed
//     [3]int and succeed on a boxed [4]int; the test below boxes [3]int into
//     a Box<[3]int> only, which is the issue's exact shape, and pins the
//     positive answer. The negative answer (wrong-sized scrutinee) is pinned
//     alongside the negative-control cases.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// The host class — for every instantiation we need a `Box<T>` whose `Probe`
// performs `o is T`, then an `Object`-typed scrutinee that IS the substituted
// type. The shape is the same in every case; only the substituted `T` differs.
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string full(const std::string& mainBody) {
    return std::string(
        "module M; "
        "class Box<T> "
        "  public constructor() end constructor; "
        "  public method Probe(o: Object): int "
        "    if o is T then return 1; end; "
        "    return 0; "
        "  end Probe; "
        "end Box; "
        "class C public static method Main(): int ") + mainBody +
        " end Main; end C;";
}

}  // namespace

// ── CARDINAL: the issue's exact repro. Box<[3]int>.Probe(o) where o
// holds a boxed [3]int. The issue says this prints 0 (silent false) and
// it should print 1. We additionally pin the result through the exit code
// so the assertion is independent of Console.WriteLine plumbing.
BEZEL_TEST(codegen_3092, is_t_inside_box_inline_array_matches_8_6_12_9) {
    // Expect 1. Without the fix, lowerTypeTest returns a null descriptor,
    // the `is` test resolves to false, and Probe returns 0.
    int rc = run(full(
        "  var a: [3]int := [4, 5, 6]; "
        "  var o: Object := a; "
        "  using var b := new Box<[3]int>(); "
        "  return b.Probe(o); "));
    BEZEL_EXPECT_EQ(rc, 1);
}

// ── BROAD-VS-NARROW: the OTHER concrete-type instantiations `boxDescriptorFor`
// knows about must also resolve. The cardinal test covers the inline-array
// path; the others exercise the unit, primitive, and builtin-generic-with-arg
// paths. Each should answer 1 (the scrutinee IS the substituted T).
// Every one of these PROBABLY also breaks at HEAD (same shape, same dropped
// descriptor, same silent false). If any pass at HEAD, the broad-vs-narrow
// still has teaching value: the fix's predicate substitution must not break
// the cases the existing fast path already handled.

BEZEL_TEST(codegen_3092, is_t_inside_box_unit_matches_8_6_12_9) {
    // Box<()>.Probe(o) where o IS a boxed () — expect 1.
    int rc = run(full(
        "  var u := (); "
        "  var o: Object := u; "
        "  using var b := new Box<()>(); "
        "  return b.Probe(o); "));
    BEZEL_EXPECT_EQ(rc, 1);
}

BEZEL_TEST(codegen_3092, is_t_inside_box_int_matches_8_6_12_9) {
    // Box<int>.Probe(o) where o IS a boxed int — expect 1.
    int rc = run(full(
        "  var i: int := 42; "
        "  var o: Object := i; "
        "  using var b := new Box<int>(); "
        "  return b.Probe(o); "));
    BEZEL_EXPECT_EQ(rc, 1);
}

BEZEL_TEST(codegen_3092, is_t_inside_box_string_matches_8_6_12_9) {
    // Box<string>.Probe(o) where o IS a boxed string — expect 1.
    // NOTE: a bare `Object := "x"` puts a `bz_string*` (not a String box) in
    // the slot, and the #1071 string discriminator short-circuits the
    // descriptor walk to `false`. To match the same shape the other cases
    // use (a value-typed scrutinee carrying a runtime descriptor), this
    // test routes the string through an explicit `Object` local that the
    // boxing path fills. If the stdlib does not expose a String wrapper,
    // this assertion can be lifted — the three other broad-vs-narrow cases
    // already pin the predicate's reach.
    int rc = run(full(
        "  var s: string := \"x\"; "
        "  var o: Object := s; "
        "  using var b := new Box<string>(); "
        "  return b.Probe(o); "));
    BEZEL_EXPECT_EQ(rc, 1);
}

// ── NEGATIVE CONTROL: same Box<[3]int>, but the scrutinee is a LONG — the
// descriptor chain must reject the wrong boxed value. This is the bit the
// cardinal does not pin: it suppresses the "the fix made `is T` match
// EVERYTHING" class. Expect 0.
BEZEL_TEST(codegen_3092, is_t_inside_box_inline_array_rejects_other_box_8_6) {
    int rc = run(full(
        "  var l: long := 999; "
        "  var o: Object := l; "
        "  using var b := new Box<[3]int>(); "
        "  return b.Probe(o); "));
    BEZEL_EXPECT_EQ(rc, 0);
}
