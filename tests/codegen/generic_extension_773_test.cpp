// fix #773 / §26.5: extension blocks on user-defined GENERIC class types are
// matched against receivers of that type and dispatch correctly.
//
// §26.5: a generic extension `extension Bag<T> where T: class` applies to any
// `Bag<X>` — the free type variable `T` unifies with the receiver's argument.
// Previously the extension-target matcher deferred generic targets entirely, so
// `b.IsEmpty()` on a `Bag<Widget>` reported "no member IsEmpty" (FALSE_REJECT).
//
// UPDATE (fix #4233, §26.4/§26.5, v0.298): the SCOPE note below described a
// method whose SIGNATURE references the free type variable (`FirstOr(x: T):
// T`) as kept cleanly REJECTED, pending per-instantiation T-substitution in
// codegen. That substitution was already unconditional since #714 (it keys
// off the free-var BINDING recorded on the call, not off whether the
// extension's target is builtin) — the reject here was a stale, unnecessary
// restriction rather than a real gap, and #4233 removed it:
// `free_var_in_signature_rejected_cleanly` below is now
// `free_var_in_signature_resolves_and_runs_4233`, asserting the call
// resolves AND returns the correct value (not just "compiles"). A sibling
// control, `free_var_in_signature_wrong_arg_type_still_rejected_4233`, pins
// that argument-type soundness is enforced against the SUBSTITUTED
// parameter type — a call passing an argument of the wrong concrete type
// must still be a compile error, not a silently-accepted type mismatch.
//
// A method that uses T only via the receiver's own members (`IsEmpty(): bool`)
// remains fully supported, as before.
//
// #1276 update: `concrete_generic_extension_not_misapplied` below was
// corrected — its original `extension Box<W>` example is actually a free
// (shadowing) type parameter per §26.4, not a concrete reference to the
// class `W`; see that test's comment.
//
// #4244 correction (send-back on #4233's review, see the three
// `combine_style_body_*_4244` tests at the end of this file): #4244 flagged
// that #4233 widens exposure to the pre-existing §3/#605 permissive-member-
// access backstop and claimed a Combine-shaped body silently returns 0
// instead of 7. MEASURED across 2 oracles (plain build, `--sanitize=
// address`) x multiple shapes (verbatim repro, non-zero field offset, two
// mixed instantiations, argv-sourced non-foldable values): the value is
// correct in every row; only the `#2491` "degraded to zero" diagnostic is a
// false positive (filed as #4248). One of #4244's own measurements (a
// transcript showing exit 0) remains UNRECONCILED with this — see the
// commit and #4244's comment thread for the full, honest account; do not
// take the tests below as proof that discrepancy is explained, only that it
// was not reproducible under the conditions tested here.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
bool rejected(const std::string& program) {
    return !runAot(program).ran;
}

// `Bag<T>` (a generic class) + an `extension Bag<T>` declaring `body`.
// §28.5: `_items` stores a freshly-`new`ed owning collection, so the field is
// `owned` (the AOT path runs the ownership checker the in-process JIT harness
// skipped; a plain field would leak and is a compile error). The compiler
// synthesizes disposal for the owned field, discharged by the `using var b`.
std::string prog(const std::string& ext, const std::string& mainBody) {
    return std::string(
        "module M; "
        "class Bag<T> where T: class "
        "  private owned _items: Array<T>; "
        "  public constructor() _items := new Array<T>(); end; "
        "  public method Add(x: T): () _items.Add(x); end; "
        "  public method Count(): int => _items.Count; "
        "end Bag; "
        "class W public v: int; public constructor(n: int) v := n; end; end W; ") +
        ext +
        " public static class P public static method Main(): int " + mainBody +
        " end Main; end P;";
}
}  // namespace

// §26.5: a generic extension's no-free-var method dispatches on a concrete
// receiver. `IsEmpty()` uses T only via the receiver's `Count()`. (The repro.)
BEZEL_TEST(generic_extension_773, no_free_var_method_dispatches) {
    BEZEL_EXPECT_EQ(runFull(prog(
        "public extension Bag<T> where T: class "
        "  public method IsEmpty(): bool => this.Count() = 0; "
        "end; ",
        "  using var b := new Bag<W>(); "
        "  return (if b.IsEmpty() then 0 else 1); ")), 0);  // empty -> IsEmpty true
}

// §26.5: after adding an element, the extension method observes the receiver's
// updated state (dispatch is on the live receiver, not a stale copy).
BEZEL_TEST(generic_extension_773, method_sees_receiver_state) {
    BEZEL_EXPECT_EQ(runFull(prog(
        "public extension Bag<T> where T: class "
        "  public method IsEmpty(): bool => this.Count() = 0; "
        "end; ",
        "  using var b := new Bag<W>(); "
        "  b.Add(new W(1)); "
        "  return (if not b.IsEmpty() then 0 else 1); ")), 0);  // non-empty
}

// fix #4233 (§26.4/§26.5, v0.298): a method whose signature (here, both the
// parameter AND the return type) mentions the extension's free var now
// resolves AND runs — not merely "does not crash": `FirstOr(f)` must return
// `f` itself (`fallback`), so `r.v` is `f.v` (9), proving the free var was
// substituted with the receiver's actual concrete argument (`W`), not just
// accepted structurally. Was `free_var_in_signature_rejected_cleanly`,
// asserting `rejected(...)`; #4233 made that assertion FALSE (see this
// file's fix #4233 header note) — replaced rather than deleted so the
// SHAPE that used to be deferred stays covered.
BEZEL_TEST(generic_extension_773, free_var_in_signature_resolves_and_runs_4233) {
    BEZEL_EXPECT_EQ(runFull(prog(
        "public extension Bag<T> where T: class "
        "  public method FirstOr(fallback: T): T => fallback; "
        "end; ",
        "  using var b := new Bag<W>(); "
        "  using var f := new W(9); "
        "  var r := b.FirstOr(f); "
        "  return r.v; ")), 9);
}

// fix #4233 (§26.4/§26.5, v0.298) SOUNDNESS CONTROL: now that a param typed
// by the extension's free var resolves, an argument of the WRONG concrete
// type must still be a compile error — the free var is substituted to the
// receiver's bound type (`W`, via `Bag<W>`) before the §5.1 assignability
// check, so passing an unrelated class (`Other`, not `W` and not a `W`
// supertype) is rejected exactly as an ordinary `W`-typed parameter would
// reject it. This is the accept-side control for #4233: the fix must not
// trade a false reject for a false accept.
BEZEL_TEST(generic_extension_773,
           free_var_in_signature_wrong_arg_type_still_rejected_4233) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; "
        "class Bag<T> where T: class "
        "  private owned _items: Array<T>; "
        "  public constructor() _items := new Array<T>(); end; "
        "  public method Add(x: T): () _items.Add(x); end; "
        "  public method Count(): int => _items.Count; "
        "end Bag; "
        "class W public v: int; public constructor(n: int) v := n; end; end W; "
        "class Other end Other; "
        "public extension Bag<T> where T: class "
        "  public method FirstOr(fallback: T): T => fallback; "
        "end; "
        "public static class P public static method Main(): int "
        "  using var b := new Bag<W>(); "
        "  using var o := new Other(); "
        "  var r := b.FirstOr(o); "
        "  return 0; "
        "end Main; end P;"));
}

// §26.5 safety: a CONCRETE generic extension does not apply to a different
// instantiation — `extension Box<int>` is not seen on a `Box<A>` receiver.
//
// #1276 correction: this test previously used `extension Box<W>` (where `W`
// is also the name of an in-scope class) as its "concrete" example and
// asserted it must NOT match `Box<A>`. That was wrong: per §26.4, "each bare
// single-segment non-nullable identifier [in an extension target's
// generic-arg position] introduces a fresh block-level type parameter —
// always, shadowing any same-named type in scope" — so `extension Box<W>`
// is exactly `extension Box<T>` with the parameter spelled `W`, not a
// reference to the class `W` (TypeResolution_Names.cpp resolveTopDecl emits
// a §26.4 shadow WARNING and always binds it as the parameter, never as the
// class). It DOES match `Box<A>`; #1276's fix corrected
// lookupExtensionMember to recognize that free variable (see
// generic_interface_extension_dispatch_1276_test.cpp for the corresponding
// coverage of the now-correct match). A genuinely CONCRETE, non-matching
// target — per §26.4 written with a keyword/primitive type, a qualified
// name, or a generic instantiation, never a bare single-segment identifier —
// is `extension Box<int>`, which this test now uses.
BEZEL_TEST(generic_extension_773, concrete_generic_extension_not_misapplied) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; "
        "class Box<T> public _v: T; "
        "  public constructor(v: T) _v := v; end; end Box; "
        "class A public n: int := 0; end A; "
        "public extension Box<int> "
        "  public method Tag(): int => 7; "
        "end; "
        "public static class P public static method Main(): int "
        "  using var b := new Box<A>(new A()); "
        "  return b.Tag(); "
        "end Main; end P;"));
}

// fix #4233 review send-back (issue #4244 correction): #4244's review flagged
// that #4233 widens which extension-method bodies are call-reachable, and
// that a body accessing a member the receiver's `where T: class` constraint
// does not promise (e.g. `a.v` — nothing in `class` requires a `v` field)
// falls through the pre-existing #605/#2547/#2491 permissive-generic-
// member-access backstop, which #4244 characterized as "silently returns 0
// instead of 7".
//
// MEASURED (send-back investigation, not assumed): that specific claim does
// NOT reproduce on this commit. Re-running #4244's own repro verbatim, with
// an oracle immune to 8-bit exit-code truncation (`Console.WriteLine` of the
// computed value, not just the process exit code) and swept across 7
// variant shapes — different arithmetic, a field at a NON-zero byte offset,
// two different concrete instantiations sharing one linked codegen symbol,
// and values sourced from `args.Count` (so LLVM cannot have constant-folded
// them) — every row returns the mathematically correct value under BOTH the
// plain build AND `--sanitize=address` (no ASan/LSan report on any of them;
// #2633's "check both oracles" applies and both agree). The `#2491`
// "degraded to zero" Warning still fires (that part of #4244 is real and
// reproduces exactly) but the value it claims is wrong is not — a
// false-positive diagnostic, filed separately as #4248, distinct from a
// genuine wrong-VALUE regression. Full per-row transcripts (both oracles) at
// /var/tmp/bezel-scratch/r18/4233-evidence/my_measurements.txt.
//
// DECISION (CLAUDE.md decline-with-reasons; see this fix's commit for the
// full record): ship #4233 as-is. Scoping the fix to reject unpromised
// member access (send-back option (a)) or promoting #2491's Warning to an
// Error (option (b)) would both mean rewriting the general, pre-existing,
// spec-referenced §3/#605 permissive-member-access policy shared across
// dozens of codegen call sites well beyond extensions (repro 2 above proves
// it fires for a plain non-extension generic method too) — out of reach
// from an extension-scoped fixback, and exactly the trade this project
// already declined once for the same reason (#4207: "trades a clear
// compile-time rejection for a silently wrong runtime answer, which is a
// regression in kind, not a completion") for a case that, unlike this one,
// really did just return the wrong value.
//
// This test pins the corrected, measured ACCEPT-side value (not the wrong
// answer #4244 reported) — do not read this as "the #2491 diagnostic is
// fixed"; it still fires and is tracked separately as #4248.
BEZEL_TEST(generic_extension_773, combine_style_body_computes_correct_value_4244) {
    BEZEL_EXPECT_EQ(runFull(prog(
        "public extension Bag<T> where T: class "
        "  public method Combine(a: T, b: T): int => a.v + b.v; "
        "end; ",
        "  using var b := new Bag<W>(); "
        "  using var w1 := new W(3); "
        "  using var w2 := new W(4); "
        "  return b.Combine(w1, w2); ")), 7);
}

// fix #4233 review send-back (issue #4244) SOUNDNESS CONTROL 1: even for a
// Combine-shaped (non-pass-through) body that touches the #605 backstop
// internally, the CALL-SITE arity check #4233 relies on
// (checkCallArity/bindArgsToParams, §26.5) is unaffected by what the body
// does — a wrong-arity call is still cleanly rejected before the body is
// ever reached. Companion to combine_style_body_computes_correct_value_4244
// above; not a new code path, this pins that #4233's own machinery stays
// sound for a body shape richer than the pass-through `FirstOr` this file's
// other #4233 tests use.
BEZEL_TEST(generic_extension_773,
           combine_style_body_wrong_arity_still_rejected_4244) {
    BEZEL_EXPECT_TRUE(rejected(prog(
        "public extension Bag<T> where T: class "
        "  public method Combine(a: T, b: T): int => a.v + b.v; "
        "end; ",
        "  using var b := new Bag<W>(); "
        "  using var w1 := new W(3); "
        "  return b.Combine(w1); ")));
}

// fix #4233 review send-back (issue #4244) SOUNDNESS CONTROL 2: the
// argument-type check #4233 added (checkArgsAgainstParams substituting the
// extension's free-var binding before §5.1 assignability) also stays sound
// for the Combine-shaped body — an argument of the wrong concrete type is
// still a compile error, not silently accepted alongside the body's own
// (separately tracked, #4248) diagnostic-accuracy issue.
BEZEL_TEST(generic_extension_773,
           combine_style_body_wrong_arg_type_still_rejected_4244) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; "
        "class Bag<T> where T: class "
        "  private owned _items: Array<T>; "
        "  public constructor() _items := new Array<T>(); end; "
        "  public method Add(x: T): () _items.Add(x); end; "
        "  public method Count(): int => _items.Count; "
        "end Bag; "
        "class W public v: int; public constructor(n: int) v := n; end; end W; "
        "class Other end Other; "
        "public extension Bag<T> where T: class "
        "  public method Combine(a: T, b: T): int => a.v + b.v; "
        "end; "
        "public static class P public static method Main(): int "
        "  using var b := new Bag<W>(); "
        "  using var w1 := new W(3); "
        "  using var o := new Other(); "
        "  return b.Combine(w1, o); "
        "end Main; end P;"));
}
