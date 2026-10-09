// Spec-derived regression test for issue #4582 — a static member access
// through a generic type (`Fac<string>.Make(…)`) recorded NO
// `GenericInstantiation`, which structurally disarmed every §3.6 rule-2
// "Pass 2" check at that one spelling, IN A SINGLE MODULE.
//
// THE GOVERNING RULE. Spec §3.6 rule 2 "Per-instantiation evaluation for
// generics" (v0.306): "For a generic type or member, the property is evaluated
// at **each instantiation**, after substituting the type arguments … an
// instantiation that violates a property-dependent requirement is rejected *at
// the instantiation site*, with a diagnostic naming the property, the offending
// type argument, and the structural reason (§28.5's generic-owned-field check
// and the owning-collection element restriction of §14.4 follow this pattern)."
//
// `Fac<string>` is an instantiation of `Fac<T>` whichever way it is spelled.
// The rule names no spelling, so a receiver spelling is as much an
// instantiation site as a typed local, a field, a parameter, a return type or
// a `new`. §14.4 ("Class-only restriction on owning generics") and §28.5 (the
// generic `owned`-field restriction) are the two patterns that sentence names
// explicitly, and both are among the checks that were disarmed.
//
// ROOT CAUSE. `Resolver::resolveNamed` is the ONLY place that pushed into
// `instantiations_`, and it runs off an `ast::Type` node. The receiver of a
// static member access has none: `Fac<string>.Make(x)` parses as a CallExpr
// whose callee is a MemberAccessExpr whose target is an `ast::IdentifierExpr`
// carrying `typeArgs`. The §14.2 constraint check at that spelling (#3160)
// therefore had to be bolted on from ExprTypecheck through a TEMPORARY
// `Resolver` (`checkGenericConstraintsAtExpr`), which by construction cannot
// write into the real Resolver's `instantiations_` — the same obstacle #4465
// met and worked around by flushing that temporary's deferred list.
//
// THE FIX — RECORD, not a seventh workaround. `instantiations_` is read by
// EIGHT independent checks, so a spelling that does not record there is
// silently exempt from all seven; one workaround per check is exactly how this
// family acquired its holes. `Resolver::resolveTypesInExpr`'s IdentifierExpr
// arm already resolves the receiver's type arguments (#520) and already runs
// inside `resolveTopDecl`, which `Resolver::run` completes BEFORE it runs any
// Pass-2 check — so the record can simply be made there, with no temporary
// Resolver at all (`Resolver::recordStaticReceiverInstantiation`, feeding the
// shared `Resolver::recordGenericInstantiation` extracted out of
// `resolveNamed`). `GenericInstantiation` gained `siteKey`/`loc` because the
// old `site` field was an `ast::Type*` used only as a dedup key and a
// diagnostic anchor, and this spelling has no such node.
//
// MEASURED ON `cafdfc52` (this lane's base = origin/main), one module, with
// `build/bezelc`. Each row below was probed in TWO spellings of the very same
// program: the type-position control, which was already rejected, and the
// static-receiver spelling, which was ACCEPTED with zero diagnostics at any
// severity. All six checks the issue names were confirmed disarmed:
//
//   check (Pass 2 over instantiations_)          type-pos   static-recv
//   §14.4 checkOwningElementInstantiations       REJECT     ACCEPT  <- UAF
//   §28.5 owned-field (inline in resolveNamed)   REJECT     ACCEPT
//   §17.4 checkAsTargetInstantiations            REJECT     ACCEPT
//   §3.6/§8.3 checkHashContainerKeys      Pass2  REJECT     ACCEPT
//   §28.11 checkElementCategoryInstantiations    REJECT     ACCEPT
//   §14.4/§15.3 checkOwningBufferSpanMoveOnly…   REJECT     ACCEPT
//
// THE MEMORY-SAFETY EVIDENCE. The §14.4 row is not a tidiness gap. The issue's
// repro (kUafRepro below, verbatim) compiled with zero diagnostics and the
// program was a heap-use-after-free — measured on `cafdfc52` with
// `bezelc --sanitize=address`:
//
//   ==391153==ERROR: AddressSanitizer: heap-use-after-free
//     READ of size 8 at 0x7bbd73fe00a8
//     #0 bz$S5L.C$Round
//   freed by thread T0 here:
//     #1 bz$S5L.C$Round        (via bz_string_release)
//
// That is the #2713 mechanism reached through the unrecorded spelling: codegen's
// `elementPolicyFor()` falls back to the Value row for a runtime-managed element
// in the owning family, so the collection stores the pointer without taking a
// reference while the `owned` argument transfer releases the caller's. Post-fix
// the program is REJECTED at compile time, which is how the hazard is closed —
// so that row asserts the specific §14.4 diagnostic (a bare
// `failedAtCompileTimeWithRealLocation` would pass on BOTH sides of a crash
// fix), and the sanitizer oracle is spent on the ACCEPT control instead.
//
// ⚠ FALSE-REJECT IS THE MAIN HAZARD of a fix that turns accepts into rejects,
// and this one records at a spelling nothing recorded before — so the accept
// controls below cover the shapes the new recorder must NOT disturb: a legal
// class-element static factory that actually runs and disposes, a generic
// METHOD call spelled with explicit type arguments (`M<int>(…)` — an
// IdentifierExpr with typeArgs that is NOT a type receiver, the #2736(a)
// residual the recorder must skip), an arity-mismatched spelling, and a
// receiver whose argument is perfectly well-formed.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// True iff compilation failed with an ERROR diagnostic carrying `needle`.
// Deliberately NOT a bare "did it reject": `runAot` synthesizes an Error
// diagnostic for a runtime SIGSEGV too, so a coarse helper cannot tell a
// genuine rejection from a crash — and the pre-fix behaviour of the §14.4 row
// is precisely a memory-safety failure at run time.
bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// True iff the program compiles, links, runs and exits with `expected`.
bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// ── §14.4 — the issue's own repro, VERBATIM ────────────────────────────────
// Pre-fix: zero diagnostics, heap-use-after-free at run time (trace above).
const char* kUafRepro =
    "module S5L; "
    "public class Fac<T> where T: class "
    "  public static method Make(owned x: T): owned OwningLinkedList<T> "
    "    var l := new OwningLinkedList<T>(); "
    "    l.AddLast(move x); "
    "    return l; "
    "  end Make; "
    "end Fac; "
    "class C "
    "  public static method Round(i: int): int "
    "    using var l := Fac<string>.Make($\"value-{i}\"); "
    "    var f := l.First; "
    "    if f <> null then "
    "      return f!.Length; "
    "    end; "
    "    return 0; "
    "  end Round; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var j in 1..20 do "
    "      t := t + C.Round(j); "
    "    end; "
    "    return t; "
    "  end Main; "
    "end C;";

// ── ACCEPT control, run under ASan/LSan ────────────────────────────────────
// The legitimate shape the fix must not disturb: the SAME static-factory
// spelling at a proper class element type, actually exercising owning-element
// disposal. Value: 6 rounds x 3 elements = 18.
const char* kClassElementStaticFactory =
    "module M; "
    "class Res public n: int := 0; end Res; "
    "public class Fac<T> where T: class "
    "  public static method Make3(owned a: T, owned b: T, owned c: T): "
    "      owned OwningLinkedList<T> "
    "    var l := new OwningLinkedList<T>(); "
    "    l.AddLast(move a); "
    "    l.AddLast(move b); "
    "    l.AddLast(move c); "
    "    return l; "
    "  end Make3; "
    "end Fac; "
    "class C "
    "  public static method Round(): int "
    "    using var l := Fac<Res>.Make3(new Res(), new Res(), new Res()); "
    "    return l.Count; "
    "  end Round; "
    "  public static method Main(): int "
    "    var t := 0; "
    "    for var i in 1..6 do "
    "      t := t + C.Round(); "
    "    end; "
    "    return t; "
    "  end Main; "
    "end C;";

}  // namespace

// =============================================================================
// REJECT — each of the six §3.6 rule-2 checks, at the static-receiver
// spelling. Every row asserts the diagnostic TEXT, never a bare rejection.
// =============================================================================

// §14.4 / §3.6 rule 2, v0.306 — the issue's measured heap-use-after-free.
// Rejecting this instantiation is what closes the hazard.
BEZEL_TEST(sema_static_receiver_generic_inst_4582,
           owning_element_via_static_receiver_rejected_4582) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        kUafRepro, "requires a class or interface element type"));
    // Pin the §3.6 rule-2 per-instantiation discipline and the bound argument,
    // so the row cannot be satisfied by the DECLARATION-site message (which
    // carries neither).
    BEZEL_EXPECT_TRUE(rejectsWith(kUafRepro, "3.6 rule 2"));
    BEZEL_EXPECT_TRUE(rejectsWith(kUafRepro, "is bound to `string`"));
}

// §28.5 / §3.6 rule 2, v0.306 — a generic class with an `owned T` field admits
// T = class/interface only; `where T: class` over-admits `string` (§14.2/§28.9).
BEZEL_TEST(sema_static_receiver_generic_inst_4582,
           owned_field_string_via_static_receiver_rejected_4582) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "public class HolderO<T> where T: class "
        "  public owned f: T?; "
        "  public static method Ping(): int return 1; end Ping; "
        "end HolderO; "
        "class C public static method Main(): int "
        "  return HolderO<string>.Ping(); "
        "end Main; end C;",
        "may not be instantiated with `string` for type parameter `T`"));
}

// §17.4 / §3.6 rule 2, v0.306 — a body using the parameter as an `as` target
// is rejected at an instantiation that makes the target degenerate. `()` has
// exactly one value, so `x as ()` can never be informative.
BEZEL_TEST(sema_static_receiver_generic_inst_4582,
           as_target_unit_via_static_receiver_rejected_4582) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "public class Caster<T> "
        "  public static method Ping(): int return 1; end Ping; "
        "  public method Try(o: Object): int "
        "    var r := o as T; "
        "    if r <> null then return 1; end; "
        "    return 0; "
        "  end Try; "
        "end Caster; "
        "class C public static method Main(): int "
        "  return Caster<()>.Ping(); "
        "end Main; end C;",
        "is used as an `as` target in the generic body"));
}

// §3.6 / §8.3, v0.306 — `checkHashContainerKeys` Pass 2: the interior
// `Dictionary<K, int>` obligation re-checked against the concrete binding.
// `Holder` is a record that is neither auto-hashable nor equality-declaring.
BEZEL_TEST(sema_static_receiver_generic_inst_4582,
           hash_container_key_via_static_receiver_rejected_4582) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record Holder(item: R); "
        "class Registry2<K> "
        "  private owned f: Dictionary<K, int> := new Dictionary<K, int>(); "
        "  public method N(): int return f.Count; end N; "
        "  public static method Ping(): int return 1; end Ping; "
        "end Registry2; "
        "class C public static method Main(): int "
        "  return Registry2<Holder>.Ping(); "
        "end Main; end C;",
        "requires a hashable key type"));
}

// §28.11 / §15.3, v0.306 — `checkElementCategoryInstantiations` Pass 2: the
// element-slot-policy table's move-only row is "rejected at instantiation" for
// BOTH collection families, and §35.4 rule 1 makes `Wrap` (a case with an
// `owned` payload) an owning-classified, hence move-only, choice.
BEZEL_TEST(sema_static_receiver_generic_inst_4582,
           element_category_move_only_via_static_receiver_rejected_4582) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "public class Conn public id: int; "
        "  public constructor(id: int) this.id := id; end constructor; "
        "end Conn; "
        "public choice Wrap Item(owned conn: Conn), None; end Wrap; "
        "public class Reg<V> "
        "  public d: Dictionary<int, V>?; "
        "  public static method Ping(): int return 1; end Ping; "
        "end Reg; "
        "class C public static method Main(): int "
        "  return Reg<Wrap>.Ping(); "
        "end Main; end C;",
        "as its V type argument"));
}

// §14.4 / §15.3, v0.306 — `checkOwningBufferSpanMoveOnlyInstantiations`:
// OwningBuffer/Span/ReadOnlySpan admit value-typed T as an explicit §14.4
// exception, but still reject the two move-only kinds of §15.3.
BEZEL_TEST(sema_static_receiver_generic_inst_4582,
           owning_buffer_move_only_via_static_receiver_rejected_4582) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "public class Conn public id: int; "
        "  public constructor(id: int) this.id := id; end constructor; "
        "end Conn; "
        "public choice Wrap Item(owned conn: Conn), None; end Wrap; "
        "public class H<T> "
        "  public b: OwningBuffer<T>?; "
        "  public static method Ping(): int return 1; end Ping; "
        "end H; "
        "class C public static method Main(): int "
        "  return H<Wrap>.Ping(); "
        "end Main; end C;",
        "still rejects move-only value types"));
}

// =============================================================================
// ACCEPT controls — the false-reject hazard of a recording fix.
// =============================================================================

// §14.4, v0.306 — the legitimate static-factory spelling at a class element
// type still compiles, runs, and is sanitizer-clean. This is the row that
// carries the memory-safety oracle now that the UAF repro no longer compiles.
BEZEL_TEST(sema_static_receiver_generic_inst_4582,
           class_element_static_factory_runs_clean_4582) {
    // NO `if (wasSkipped(r)) return;` guard: that shape is the #3733
    // toolchain-SKIP absorber `scripts/check_toolchain_skip_surface.py`
    // ratchets down — on a box without clang it turns the whole sanitizer
    // tier into a silent green. The harness reports a missing toolchain
    // loudly instead (`CHECKED TOOLCHAIN: N skip(s)` plus the run_parallel
    // floor), so asserting unconditionally is the fail-LOUD direction.
    JitResult r = runAotChecked(kClassElementStaticFactory);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 18);
}

// §3.6 rule 2, v0.306 — a well-formed static-receiver instantiation is still
// accepted. The recorder records EVERY arity-matched receiver, so the plain
// case must stay silent.
BEZEL_TEST(sema_static_receiver_generic_inst_4582,
           wellformed_static_receiver_still_accepted_4582) {
    BEZEL_EXPECT_TRUE(runs(
        7,
        "module M; "
        "public class Box<T> "
        "  public static method Seven(): int return 7; end Seven; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box<int>.Seven(); "
        "end Main; end C;"));
}

// §14.6 / #2736(a), v0.306 — a generic METHOD call spells an IdentifierExpr
// with `typeArgs` too (`Id<int>(5)`), but its parameter belongs to the METHOD,
// not to a type: `instantiations_` does not model those bindings at all. The
// recorder must skip it (the name does not resolve to a TypeSymbol) rather
// than record a bogus node against some same-named type.
BEZEL_TEST(sema_static_receiver_generic_inst_4582,
           generic_method_call_not_recorded_as_type_instantiation_4582) {
    BEZEL_EXPECT_TRUE(runs(
        5,
        "module M; "
        "class C "
        "  public static method Id<U>(u: U): U return u; end Id; "
        "  public static method Main(): int "
        "    return C.Id<int>(5); "
        "  end Main; "
        "end C;"));
}

// §14.4, v0.306 — an UNINSTANTIATED generic body is not an instantiation: a
// static receiver spelled with the enclosing generic's OWN parameter
// (`Inner<T>.Ping()` inside `Outer<T>`) binds nothing concrete, so the sweep
// must stay silent until a concrete outer instantiation exists — and when one
// does exist at a good argument, it must still be accepted.
BEZEL_TEST(sema_static_receiver_generic_inst_4582,
           bare_parameter_receiver_chains_but_does_not_overreject_4582) {
    BEZEL_EXPECT_TRUE(runs(
        3,
        "module M; "
        "class Res public n: int := 0; end Res; "
        "public class Inner<T> where T: class "
        "  public static method Make(): owned OwningLinkedList<T> "
        "    return new OwningLinkedList<T>(); "
        "  end Make; "
        "end Inner; "
        "public class Outer<T> where T: class "
        "  public static method Go(): int "
        "    using var l := Inner<T>.Make(); "
        "    return l.Count + 3; "
        "  end Go; "
        "end Outer; "
        "class C public static method Main(): int "
        "  return Outer<Res>.Go(); "
        "end Main; end C;"));
}

// §14.4 / §3.6 rule 2, v0.306 — the CHAINED form of the row above: the outer
// generic forwards its own parameter to the inner one through a static
// receiver, and the outer is then instantiated at `string`. Pass 2's
// re-keying worklist must follow the chain through the newly recorded
// `Inner<T>` node, so the rejection still lands.
BEZEL_TEST(sema_static_receiver_generic_inst_4582,
           chained_bare_parameter_receiver_rejected_at_outer_binding_4582) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M; "
        "public class Inner<T> where T: class "
        "  public static method Make(): owned OwningLinkedList<T> "
        "    return new OwningLinkedList<T>(); "
        "  end Make; "
        "end Inner; "
        "public class Outer<T> where T: class "
        "  public static method Go(): int "
        "    using var l := Inner<T>.Make(); "
        "    return l.Count; "
        "  end Go; "
        "end Outer; "
        "class C public static method Main(): int "
        "  return Outer<string>.Go(); "
        "end Main; end C;",
        "requires a class or interface element type"));
}
