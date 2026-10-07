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
//
// #5449 update (f28e3406, §3.6 rule 2 / §14.2, v0.315): "a bare type
// parameter's member surface is what its constraints establish, never the
// concrete substitution's." The `where T: class` Combine-shaped rows below
// read `a.v` on a bare `T` that no constraint provides `v`, so they are now
// REJECTED AT THE DECLARATION SITE (bezelc exits 1, no binary) rather than
// computing a value. Each such row is split into the `*_rejected` half (must
// not run, and must carry the §3.6 rule 2 / §14.2 diagnostic naming `T` and
// `v`) and a `*_constrained` working twin carrying the remedy `where T: W` —
// the same pairing, and for the same reason, as
// generic_record_field_590_test.cpp. Two soundness controls were additionally
// going VACUOUS on #5449 (the declaration-site rejection made `rejected()`
// true regardless of the arity / argument-type error each row exists to pin);
// they are now constrained AND assert the specific §26.5 / §5.1 diagnostic.
// The spurious `#2491` Warning is unchanged and still tracked as #4248.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains,
                                            // failedAtCompileTimeWithRealLocation
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

// #5449 (f28e3406) rejects member access on a bare type parameter at the
// DECLARATION site when the member is not in the constraint-derived surface
// (§3.6 rule 2 / §14.2, v0.315). Asserting on the message (not merely `!ran`)
// keeps the rows below load-bearing: a program that failed for an UNRELATED
// reason would not satisfy them.
void expectBareTypeParamRejection(const std::string& program,
                                  const std::string& member) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);           // the program never ran
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("bare type parameter") != std::string::npos &&
            d.message.find("`T`") != std::string::npos &&
            d.message.find("`" + member + "`") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);            // names T and the missing member
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
// This row now pins the REJECT-side outcome #5449 established for exactly this
// program (a bare `T` whose constraint never promised `v`): the declaration-site
// §3.6 rule 2 / §14.2 diagnostic. Its ACCEPT-side counterpart, the measured
// value-7 row that refuted #4244's wrong-value claim, is
// combine_style_body_computes_correct_value_constrained_4244 below — same body,
// with the remedy constraint. Do not read either as "the #2491 diagnostic is
// fixed"; it still fires and is tracked separately as #4248.
BEZEL_TEST(generic_extension_773, combine_style_body_rejected_4244) {
    expectBareTypeParamRejection(prog(
        "public extension Bag<T> where T: class "
        "  public method Combine(a: T, b: T): int => a.v + b.v; "
        "end; ",
        "  using var b := new Bag<W>(); "
        "  using var w1 := new W(3); "
        "  using var w2 := new W(4); "
        "  return b.Combine(w1, w2); "),
        "v");
}

// WORKING TWIN of combine_style_body_rejected_4244: the same Combine-shaped
// non-pass-through body with the constraint #5449's remedy names
// (`where T: W`, W declaring `v`) added, so §14.2 puts `v` in `T`'s surface
// and the read resolves at the DECLARATION site. Preserves the coverage
// combine_style_body_computes_correct_value_4244 existed for — that a
// Combine-shaped body computes the right value rather than 0 — at the shape
// the spec admits.
//
// MEASURED that this value is genuine, not a backstop artifact landing on 0:
// with `W(3)`/`W(4)` it is 7, and with `W(30)`/`W(40)` it is 70, so a
// degraded-to-zero slot would fail the row rather than pass it. The spurious
// `#2491` "degraded to zero" internal Warning still fires on this shape and is
// tracked separately as #4248 — unchanged by this row; see the file header.
BEZEL_TEST(generic_extension_773,
           combine_style_body_computes_correct_value_constrained_4244) {
    BEZEL_EXPECT_EQ(runFull(prog(
        "public extension Bag<T> where T: W "
        "  public method Combine(a: T, b: T): int => a.v + b.v; "
        "end; ",
        "  using var b := new Bag<W>(); "
        "  using var w1 := new W(3); "
        "  using var w2 := new W(4); "
        "  return b.Combine(w1, w2); ")), 7);
}

// fix #4233 review send-back (issue #4244) SOUNDNESS CONTROL 1: even for a
// Combine-shaped (non-pass-through) body, the CALL-SITE arity check #4233
// relies on (checkCallArity/bindArgsToParams, §26.5) is unaffected by what
// the body does — a wrong-arity call is still cleanly rejected before the
// body is ever reached. Companion to the `combine_style_body_*_4244` rows
// above; not a new code path, this pins that #4233's own machinery stays
// sound for a body shape richer than the pass-through `FirstOr` this file's
// other #4233 tests use.
//
// #5449: constrained to `where T: W`. Unconstrained, this row went VACUOUS on
// #5449 — `a.v` on a bare `T` is rejected at the declaration site whatever the
// call site passes, so `rejected()` stayed true even if the arity check were
// deleted. With the constraint the body is legal, so the rejection this row
// asserts is now the §26.5 arity diagnostic and nothing else.
BEZEL_TEST(generic_extension_773,
           combine_style_body_wrong_arity_still_rejected_4244) {
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        runAot(prog(
            "public extension Bag<T> where T: W "
            "  public method Combine(a: T, b: T): int => a.v + b.v; "
            "end; ",
            "  using var b := new Bag<W>(); "
            "  using var w1 := new W(3); "
            "  return b.Combine(w1); ")),
        "takes 1 argument(s)"));
}

// fix #4233 review send-back (issue #4244) SOUNDNESS CONTROL 2: the
// argument-type check #4233 added (checkArgsAgainstParams substituting the
// extension's free-var binding before §5.1 assignability) also stays sound
// for the Combine-shaped body — an argument of the wrong concrete type is
// still a compile error, not silently accepted alongside the body's own
// (separately tracked, #4248) diagnostic-accuracy issue.
//
// #5449: constrained to `where T: W` for the same reason as SOUNDNESS CONTROL
// 1 above — unconstrained, the declaration-site bare-`T` rejection made this
// row vacuous. Asserting on the §5.1 message (not merely `rejected()`) keeps
// it load-bearing: an unrelated rejection would not satisfy it.
BEZEL_TEST(generic_extension_773,
           combine_style_body_wrong_arg_type_still_rejected_4244) {
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        runAot(
        "module M; "
        "class Bag<T> where T: class "
        "  private owned _items: Array<T>; "
        "  public constructor() _items := new Array<T>(); end; "
        "  public method Add(x: T): () _items.Add(x); end; "
        "  public method Count(): int => _items.Count; "
        "end Bag; "
        "class W public v: int; public constructor(n: int) v := n; end; end W; "
        "class Other end Other; "
        "public extension Bag<T> where T: W "
        "  public method Combine(a: T, b: T): int => a.v + b.v; "
        "end; "
        "public static class P public static method Main(): int "
        "  using var b := new Bag<W>(); "
        "  using var w1 := new W(3); "
        "  using var o := new Other(); "
        "  return b.Combine(w1, o); "
        "end Main; end P;"),
        "not implicitly convertible"));
}

// fix #4238 (§26.4/§26.5, v0.298): a `this.Method(...)` self-call to a
// SIBLING member of the SAME GENERIC extension block never resolved at all —
// "no extension method `Test` on type `Box` takes 1 argument(s) (§26.5)" —
// even when NEITHER member's signature mentions the extension's free type
// variable. Root cause: `this`'s static type inside a NAMED-type extension
// is deliberately left unpublished (ExprTypecheck_Exprs.cpp's #456/#2795
// comments), so `resolveMemberAccess`'s extension-lookup fallback (a `this`
// target with no `exprType`) synthesized a receiver from `memberTargetType`'s
// bare `TypeSymbol*` alone — carrying ZERO generic arguments, always,
// regardless of how many the extended type actually declares. For a
// GENERIC target (`extension Box<W>`) that 0-arg synthesized receiver could
// never structurally match the extension's own declared 1-arg head
// (`extensionTargetMatchesReceiver`'s `target->genericArgs.size() !=
// recv->genericArgs.size()` arity check), so `extensionFindMatchingAncestor`
// never matched and every candidate was silently dropped before the #4233
// free-var-substitution code path is ever reached (confirmed: the arity-
// specific diagnostic fired here comes from the SEPARATE `anySameNamedExtension`
// name-only scan in resolveMemberAccess, which runs only after the arity-aware
// `lookupExtensionMember` calls above it already returned null). Distinct
// from #4233 (a member's own SIGNATURE mentioning the free var): this
// reproduces even when neither `Test` nor `Caller` mentions `W` anywhere.
// This test pins the MEASURED result value (7 + 1 = 8), not just "compiles",
// so a call that silently degraded through the #2491/#2547 permissive-
// member-access backstop (which returns a zero-initialized value) could not
// pass this assertion.
BEZEL_TEST(generic_extension_773,
           self_call_inside_generic_extension_resolves_and_runs_4238) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box<U> end Box; "
        "extension Box<W> "
        "  public method Test(x: int): int "
        "    return x + 1; "
        "  end Test; "
        "  public method Caller(x: int): int "
        "    return this.Test(x); "
        "  end Caller; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(); "
        "    return b.Caller(7); "
        "  end Main; "
        "end C;"),
        8);
}

// fix #4238 ACCEPT-SIDE CONTROL 1: the identical self-call shape inside a
// NON-generic extension already worked (the 0-arg synthesized receiver
// happened to match the target's 0-arg declared head by coincidence) and
// must keep working after the fix. Uses a DIFFERENT input (10 -> 11) than
// the generic-target row above so the two cannot be confused for one
// another if either regresses.
BEZEL_TEST(generic_extension_773,
           self_call_inside_nongeneric_extension_control_keeps_working_4238) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box end Box; "
        "extension Box "
        "  public method Test(x: int): int "
        "    return x + 1; "
        "  end Test; "
        "  public method Caller(x: int): int "
        "    return this.Test(x); "
        "  end Caller; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    return b.Caller(10); "
        "  end Main; "
        "end C;"),
        11);
}

// fix #4238 ACCEPT-SIDE CONTROL 2: an EXTERNAL call (`b.Test(...)` from
// outside the extension, not `this.` from a sibling member) on a GENERIC
// extension target already worked — `ma.target`'s exprType is the concrete
// `Box<int>` set by `new Box<int>()`, so this call never took the `this`-
// synthesized-receiver fallback path this fix touches. Must keep working
// unchanged.
BEZEL_TEST(generic_extension_773,
           external_call_on_generic_extension_control_keeps_working_4238) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box<U> end Box; "
        "extension Box<W> "
        "  public method Test(x: int): int "
        "    return x + 1; "
        "  end Test; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(); "
        "    return b.Test(7); "
        "  end Main; "
        "end C;"),
        8);
}

// fix #4238 + #4233 COMBINED: a self-call to a sibling member whose
// signature DOES mention the extension's free type variable (`Peek(): W`)
// must resolve AND substitute correctly through the self-call path too —
// the binding `lookupExtensionMember` computes for a `this` receiver maps
// the free var to itself (no external instantiation has bound it yet
// inside the extension's own body), so `checkArgsAgainstParams`'s #4233
// substitution is a no-op here, but the CALL must still resolve at all
// (the #4238 defect this file's other new tests pin) and the field read
// through `this._v` must reach the correct storage. Pins a value (199)
// that neither a zero-initialized degrade nor a mismatched-field-offset
// read could plausibly produce.
BEZEL_TEST(generic_extension_773,
           self_call_free_var_signature_member_resolves_and_runs_4238) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box<U> "
        "  public _v: U; "
        "  public constructor(v: U) _v := v; end; "
        "end Box; "
        "extension Box<W> "
        "  public method Peek(): W => this._v; "
        "  public method CallPeek(): W => this.Peek(); "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(199); "
        "    return b.CallPeek(); "
        "  end Main; "
        "end C;"),
        199);
}

// fix #4238 SOUNDNESS CONTROL: a self-call to a member name that is not
// declared ANYWHERE (not on the extension, not on the extended type) must
// stay cleanly rejected, with a real diagnostic naming the missing member —
// the fix widens WHICH receivers the extension-lookup fallback matches, and
// must not turn "no such member" into a silent accept. Asserts the
// diagnostic TEXT (not just `hasErrors()`/`!ran`), per this run's own
// discipline that `failedAtCompileTimeWithRealLocation` alone is true for a
// compiler crash too — paired here with the real location check as well.
BEZEL_TEST(generic_extension_773,
           self_call_genuinely_undefined_member_still_rejected_4238) {
    auto r = runAot(
        "module M; "
        "class Box<U> end Box; "
        "extension Box<W> "
        "  public method Caller(x: int): int "
        "    return this.NoSuchMethod(x); "
        "  end Caller; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(); "
        "    return b.Caller(7); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "no member `NoSuchMethod` on type `Box`"));
}

// --- fix #4292 (§26.4/§26.5, v0.298) --------------------------------------
//
// #4238's self-call receiver synthesis above (the `this`/`currentExtension_`
// branch in `ExprTC::resolveMemberAccess`) supplies `lookupExtensionMember`
// a receiver whose generic argument is `currentExtension_`'s OWN declared
// free type variable, UNRESOLVED (`declaration == nullptr` — the #4229
// null-declaration shape). `extWhereClauseProvablyViolated`'s
// `groundCategorize` necessarily reads that as `GroundCat::Unknown`, and its
// documented "defer on undecidable => keep the candidate" direction then
// silently ADMITTED a sibling `where`-constrained block's member even when
// the constraint is not satisfied for the receiver's real instantiation —
// while the IDENTICAL call made externally (`b.Method()`) was correctly
// rejected on the same instantiation. This could not manifest before #4238
// (every self-call inside a generic extension failed outright for its own,
// unrelated reason), so it is a hole #4238's fix opened, not one it
// inherited.
//
// The four tests below (rowA-rowD) are additional #4238 SELF-CALL coverage
// the review measured by hand but did not commit — different axes
// (2-free-var target, a `where`-constrained same-block self-call, a
// self-call from inside a captured lambda, a self-call to a computed
// PROPERTY rather than a method). FAIL-WITHOUT / PASS-WITH, measured on
// this lane's own tree: built `bezelc` at r18-c3's base `10bb24d` (the
// commit immediately BEFORE #4238) in an isolated worktree and ran all four
// verbatim — every one is a compile-time reject there ("no extension
// method ... (§26.5)" / "no member `Value` on type `Box`"), and every one
// compiles and runs to the value asserted below on this lane's patched
// tree (`bezelc --version` confirmed at each build; not merely assumed from
// the review's report). That is 4 of 4 FAIL-WITHOUT / PASS-WITH.
//
// The final two tests (the constraint-gap fix itself) are the actual
// #4292 regression pair: `unsatisfied_constraint_...` FAILS on `c5c7237`
// (this lane's own pre-#4292-fix commit — it compiles and RUNS to exit 5,
// which is the bug) and PASSES (rejected, correct diagnostic text) on the
// patched tree; `satisfied_constraint_...` is the accept-side control and
// passes identically on both (a caller block that independently declares
// the SAME `where W: class` constraint on its own free var must keep
// resolving the self-call — this is the "must not regress" shape #4292's
// own writeup calls out).

// fix #4292 coverage row A: TWO free type variables (`Pair<K, V>`), a
// self-call chain (`Wrapper` -> `ReadK`) reading a REAL field (`this._k`,
// not a constant), asserting the measured VALUE (217) so a zero-initialized
// #2491/#2547 degrade or a mismatched-field-offset read could not pass.
BEZEL_TEST(generic_extension_773, self_call_two_free_vars_reads_real_field_4292) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Pair<K, V> "
        "  public _k: K; "
        "  public _v: V; "
        "  public constructor(k: K, v: V) _k := k; _v := v; end; "
        "end Pair; "
        "extension Pair<K, V> "
        "  public method ReadK(): K => this._k; "
        "  public method Wrapper(): K => this.ReadK(); "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var p := new Pair<int, bool>(217, true); "
        "    return p.Wrapper(); "
        "  end Main; "
        "end C;"),
        217);
}

// fix #4292 coverage row B: a self-call WITHIN a `where W: class`-
// constrained extension block (`Outer` calling `Inner`, both declared in the
// SAME constrained block) — the #4238 defect blocked this shape too (the
// arity check on the synthesized 0-arg receiver failed independent of any
// `where` clause), and it must keep resolving after #4292's applicability
// re-check (`foundDecl == callerDecl` trivially satisfies its own clause).
BEZEL_TEST(generic_extension_773, self_call_inside_constrained_block_4292) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box<W> end Box; "
        "extension Box<W> where W: class "
        "  public method Inner(): int => 1; "
        "  public method Outer(): int => this.Inner() + 1; "
        "end; "
        "class SomeClass end SomeClass; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<SomeClass>(); "
        "    return b.Outer(); "
        "  end Main; "
        "end C;"),
        2);
}

// fix #4292 coverage row C: a self-call made from INSIDE a lambda that
// captures `this` (`lambda(): int => this.Test(x)`), not directly from the
// enclosing method body — a different AST shape for the same `this`-rooted
// MemberAccessExpr the #4238 synthesis handles.
BEZEL_TEST(generic_extension_773, self_call_inside_captured_lambda_4292) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box<U> end Box; "
        "extension Box<W> "
        "  public method Test(x: int): int => x + 1; "
        "  public method Caller(x: int): int "
        "    var f := lambda(): int => this.Test(x); "
        "    return f(); "
        "  end Caller; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(); "
        "    return b.Caller(7); "
        "  end Main; "
        "end C;"),
        8);
}

// fix #4292 coverage row D: a self-call to a COMPUTED PROPERTY (`Echo` reads
// `this.Value`, itself a computed property reading `this._v`), not a
// method — exercises the same synthesized-receiver fallback in
// `resolveMemberAccess` for a PropertySymbol lookup rather than a
// MethodSymbol one.
BEZEL_TEST(generic_extension_773, self_call_to_computed_property_4292) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box<U> "
        "  public _v: U; "
        "  public constructor(v: U) _v := v; end; "
        "end Box; "
        "extension Box<W> "
        "  public property Value: W => this._v; "
        "  public property Echo: W => this.Value; "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(199); "
        "    return b.Echo; "
        "  end Main; "
        "end C;"),
        199);
}

// fix #4292 (§26.4/§26.5, v0.298) — THE REGRESSION ROW: `Box<int>.Caller()`
// (an UNCONSTRAINED sibling extension block) self-calls `this.OnlyForClass()`
// declared in a DIFFERENT block carrying `where W: class`. `int` does not
// satisfy `class`, so this must be a compile error — and the IDENTICAL call
// made externally (`b.OnlyForClass()`, `external_call_...` in this file's
// earlier tests family) already is one. Pre-#4292-fix (`c5c7237`) this
// compiled and RAN, returning 5 — the traced soundness hole. Asserts the
// diagnostic TEXT, matched VERBATIM to the external call's own wording for
// the identical rejection (measured on this tree: both read "no extension
// method `OnlyForClass` on type `Box` takes 0 argument(s) (§26.5)"),
// because `failedAtCompileTimeWithRealLocation` alone is true for a
// compiler crash too (this run's own discipline).
BEZEL_TEST(generic_extension_773, unsatisfied_constraint_self_call_rejected_4292) {
    auto r = runAot(
        "module M; "
        "class Box<U> end Box; "
        "extension Box<W> where W: class "
        "  public method OnlyForClass(): int => 5; "
        "end; "
        "extension Box<W> "
        "  public method Caller(): int => this.OnlyForClass(); "
        "end; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(); "
        "    return b.Caller(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "no extension method `OnlyForClass` on type `Box` takes 0 "
           "argument(s)"));
}

// fix #4292 ACCEPT-SIDE CONTROL: the satisfied-constraint counterpart of the
// row above — `Caller`'s OWN block independently declares the SAME
// `where W: class` constraint on its own free var, so every legal
// instantiation of `Caller` is guaranteed (by `Caller`'s own declaration) to
// satisfy `OnlyForClass`'s clause too. Must keep resolving and running after
// #4292's applicability re-check — this is the "must not over-reject" shape
// the fix's own soundness argument depends on, not merely a nice-to-have.
BEZEL_TEST(generic_extension_773, satisfied_constraint_self_call_keeps_working_4292) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Box<U> end Box; "
        "extension Box<W> where W: class "
        "  public method OnlyForClass(): int => 5; "
        "end; "
        "extension Box<W> where W: class "
        "  public method Caller(): int => this.OnlyForClass(); "
        "end; "
        "class SomeClass end SomeClass; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<SomeClass>(); "
        "    return b.Caller(); "
        "  end Main; "
        "end C;"),
        5);
}

// --- fix #4306 (§26.4/§26.5, v0.298) --------------------------------------
//
// `fix #4292`'s `selfCallSiblingWhereSatisfiedByCaller` (immediately above,
// this file's own history) re-checks a sibling-block self-call against the
// CALLER block's own `where` clause, but for a `type` (interface/class
// bound) constraint item its ORIGINAL check compared `want->declaration ==
// have->declaration` — LITERAL identity of the declared bound, never
// subtype entailment. That is sound (never falsely admits) but incomplete:
// a caller whose own declared bound is a nominal SUBTYPE of the sibling
// candidate's bound (an implementer vs. the interface it implements) was
// wrongly rejected, even though every legal instantiation the caller
// admits provably satisfies the candidate's clause too. #4306 fixes the
// `type` arm to test entailment via `ExprTC::isSubtype` (the same
// declaration-based walk `groundSatisfiesTypeBound`'s ground-case `type`
// arm already uses for the analogous ground question, ExprTypecheck.cpp
// §19.6/§14.2).

// fix #4306 THE REGRESSION ROW (issue's own repro, ExprTypecheck_Calls.cpp
// `selfCallSiblingWhereSatisfiedByCaller`): `Caller`'s own block declares
// `where W: BaseImpl4306` (a class implementing `IFoo4306`); the sibling
// candidate `OnlyForFoo` declares `where W: IFoo4306`. Every legal `W` the
// caller admits (every `BaseImpl4306` or subclass) is, by `BaseImpl4306`'s
// own declaration, provably an `IFoo4306` — so this must compile and
// return 5. Pre-#4306, literal-identity rejected this
// ("no extension method `OnlyForFoo` on type `Box4306` takes 0
// argument(s) (§26.5)"), measured FAIL-WITHOUT below.
BEZEL_TEST(generic_extension_773, subtype_entailment_self_call_accepted_4306) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public interface IFoo4306 method Ping(): int; end IFoo4306; "
        "class BaseImpl4306 : IFoo4306 "
        "  public method Ping(): int => 9; "
        "end BaseImpl4306; "
        "class Box4306<U> end Box4306; "
        "extension Box4306<W> where W: IFoo4306 "
        "  public method OnlyForFoo(): int => 5; "
        "end; "
        "extension Box4306<W> where W: BaseImpl4306 "
        "  public method Caller(): int => this.OnlyForFoo(); "
        "end; "
        "class C4306 "
        "  public static method Main(): int "
        "    using var b := new Box4306<BaseImpl4306>(); "
        "    return b.Caller(); "
        "  end Main; "
        "end C4306;"),
        5);
}

// fix #4306 SOUNDNESS CONTROL — WRONG DIRECTION: the mirror of the row
// above, entailment reversed. `Caller`'s own block declares the WIDER
// bound `where W: IFoo4306B` (an interface); the sibling candidate
// `OnlyForBase` declares the NARROWER bound `where W: BaseImpl4306B` (one
// particular implementer). Not every `IFoo4306B` is a `BaseImpl4306B`, so
// this must stay rejected — proves the fix checks `have` (caller) is a
// subtype of `want` (candidate), not the reverse, which an inverted
// `isSubtype(want->declaration, have->declaration)` would have wrongly
// accepted. The call-site instantiation (`Box4306B<BaseImpl4306B>`) is
// deliberately chosen so it WOULD satisfy the narrower bound if the check
// were instantiation-based rather than block-declaration-based, isolating
// exactly the defect this control targets.
BEZEL_TEST(generic_extension_773,
           subtype_entailment_wrong_direction_still_rejected_4306) {
    auto r = runAot(
        "module M; "
        "public interface IFoo4306B method Ping(): int; end IFoo4306B; "
        "class BaseImpl4306B : IFoo4306B "
        "  public method Ping(): int => 9; "
        "end BaseImpl4306B; "
        "class Box4306B<U> end Box4306B; "
        "extension Box4306B<W> where W: BaseImpl4306B "
        "  public method OnlyForBase(): int => 5; "
        "end; "
        "extension Box4306B<W> where W: IFoo4306B "
        "  public method Caller(): int => this.OnlyForBase(); "
        "end; "
        "class C4306B "
        "  public static method Main(): int "
        "    using var b := new Box4306B<BaseImpl4306B>(); "
        "    return b.Caller(); "
        "  end Main; "
        "end C4306B;");
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "no extension method `OnlyForBase` on type `Box4306B` takes 0 "
           "argument(s)"));
}

// fix #4306 REGRESSION CONTROL — LITERAL IDENTITY (still the reflexive
// case of subtype entailment): caller and candidate declare the IDENTICAL
// interface bound. This is the shape #4292's original check already
// accepted (`want->declaration == have->declaration`); pins that #4306's
// entailment rewrite keeps accepting it (`isSubtype(X, X)` is true — the
// BFS in `ExprTC::isSubtype` checks `t == to` before walking any base),
// since this exact `type`-item identity shape had no dedicated test before
// this fix.
BEZEL_TEST(generic_extension_773,
           literal_identity_type_bound_self_call_keeps_working_4306) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "public interface IFoo4306C method Ping(): int; end IFoo4306C; "
        "class Impl4306C : IFoo4306C "
        "  public method Ping(): int => 9; "
        "end Impl4306C; "
        "class Box4306C<U> end Box4306C; "
        "extension Box4306C<W> where W: IFoo4306C "
        "  public method OnlyForFooC(): int => 5; "
        "end; "
        "extension Box4306C<W> where W: IFoo4306C "
        "  public method Caller(): int => this.OnlyForFooC(); "
        "end; "
        "class C4306C "
        "  public static method Main(): int "
        "    using var b := new Box4306C<Impl4306C>(); "
        "    return b.Caller(); "
        "  end Main; "
        "end C4306C;"),
        5);
}

// fix #4306 SOUNDNESS CONTROL — UNRELATED BOUND: caller's own declared
// bound (`IBar4306D`) is a completely unrelated interface to the
// candidate's (`IFoo4306D`) — neither is a subtype of the other. Must
// stay rejected; guards against a defective `isSubtype` call (e.g. a typo
// swapping args to an unconditional-true shape) reading as a blanket
// accept.
BEZEL_TEST(generic_extension_773,
           subtype_entailment_unrelated_bound_still_rejected_4306) {
    auto r = runAot(
        "module M; "
        "public interface IFoo4306D method Ping(): int; end IFoo4306D; "
        "public interface IBar4306D method Pong(): int; end IBar4306D; "
        "class Impl4306D : IBar4306D "
        "  public method Pong(): int => 9; "
        "end Impl4306D; "
        "class Box4306D<U> end Box4306D; "
        "extension Box4306D<W> where W: IFoo4306D "
        "  public method OnlyForFooD(): int => 5; "
        "end; "
        "extension Box4306D<W> where W: IBar4306D "
        "  public method Caller(): int => this.OnlyForFooD(); "
        "end; "
        "class C4306D "
        "  public static method Main(): int "
        "    using var b := new Box4306D<Impl4306D>(); "
        "    return b.Caller(); "
        "  end Main; "
        "end C4306D;");
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "no extension method `OnlyForFooD` on type `Box4306D` takes 0 "
           "argument(s)"));
}
