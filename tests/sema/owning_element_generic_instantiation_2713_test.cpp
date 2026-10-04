// Spec-derived regression test for issue #2713 — the owning-collection
// value-element check was DECLARATION-SITE ONLY, so an ill-formed element type
// that arrived through a generic parameter was admitted with zero diagnostics.
//
//   class H<T>
//     private owned f: OwningLinkedList<T> := new OwningLinkedList<T>();
//   end H;
//   ... new H<string>()        // pre-fix: ACCEPTED, zero diagnostics
//
// THE GOVERNING RULE. Spec §14.4 — heading "`owned` and generic type
// arguments" — under its bolded paragraph "Class-only restriction on owning
// generics" (v0.240): "Most standard-library owning types (`OwningArray<T>`,
// `OwningLinkedList<T>`, `OwningSet<T>`, `OwningStack<T>`, `OwningQueue<T>`,
// `OwningDictionary<K, V>`, …) restrict their element type T to **class and
// interface types**. Value types are rejected at the instantiation site. … the
// standard-library owning types are compiler-enforced to reject the
// runtime-managed reference types (`string`, function values)."
//
// Spec §3.6 rule 2 "Per-instantiation evaluation for generics" (v0.240) says
// where that check belongs, and names this very rule as its example: "For a
// generic type or member, the property is evaluated at each instantiation,
// after substituting the type arguments. … an instantiation that violates a
// property-dependent requirement is rejected *at the instantiation site*, with
// a diagnostic naming the property, the offending type argument, and the
// structural reason (§28.5's generic-owned-field check and **the
// owning-collection element restriction of §14.4** follow this pattern)."
//
// stdlib §4.2 (v0.240) states the same rule and its reason: "Value-typed
// elements are rejected at the instantiation site (v0.165): a value element has
// no disposal obligation … Runtime-managed reference types (`string`, function
// values) are likewise rejected."
//
// ⚠ WHY THIS IS A MEMORY-SAFETY FIX, NOT A TIDINESS FIX. Issue #2713 inferred
// that the residual was "a leak, never a double-free", reasoning from
// elementPolicyFor()'s Value-row fallback (LlvmBackend_ElementPolicy.cpp:238-250)
// installing no element disposer. That inference is WRONG, and it was measured
// wrong: the Value row also means the collection takes NO reference on insert,
// while the `owned` argument transfer still RELEASES the caller's. The slot is
// left dangling. Reading the element back out is a use-after-free. Measured on
// 07cc2e0 with the pre-fix compiler, `--probe --asan`:
//
//   ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8
//     #0 bz$M.C$Round
//   freed by thread T0 here:
//     #1 bz_string_release
//     #2 bz$M.C$Round
//
// kUafRepro below is that exact program. Post-fix it is REJECTED at compile
// time, which is how the hazard is closed — so the pinning test asserts the
// diagnostic, and the ASan oracle is spent instead on the ACCEPT control
// (kClassElementDisposes), proving the legitimate owning path still compiles,
// runs, and is sanitizer-clean.
//
// ⚠ FALSE-REJECT IS THE MAIN HAZARD of a rejection fix, so the accept controls
// below outnumber the reject cases and cover every spelling that reaches the
// predicate: a class element, an interface element, a nullable class element,
// an UNINSTANTIATED template body (where T is still a placeholder — the case a
// naive "is it a value type?" test gets backwards), a nested owning collection,
// a parameter forwarded from an outer generic to an inner one, an owning
// dictionary's deliberately value-like KEY slot, and `OwningBuffer<T>` — which
// §14.4 names as an explicit EXCEPTION admitting value-typed T.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// True iff an ERROR diagnostic is THIS defect's per-instantiation rejection.
// Deliberately NOT a bare `rejects()`: runAot synthesizes an Error diagnostic
// for a runtime SIGSEGV too, so a coarse helper cannot tell a genuine rejection
// from a crash. The three substrings pin the §14.4 property, the §3.6 rule-2
// per-instantiation discipline (which ONLY the new Pass 2 emits — the
// declaration-site message does not carry it), and the naming of the parameter.
bool hasPerInstantiationOwningError(const std::string& src,
                                    const std::string& expectedArg) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("requires a class or interface element type") ==
            std::string::npos)
            continue;
        if (d.message.find("3.6 rule 2") == std::string::npos) continue;
        if (d.message.find("type parameter") == std::string::npos) continue;
        if (d.message.find("is bound to") == std::string::npos) continue;
        if (d.message.find(expectedArg) == std::string::npos) continue;
        return true;
    }
    return false;
}

// Compiles, links, runs, and exits with `expected`.
bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// A generic holder whose owned field is `container<T>`, plus a Main that
// instantiates it with `arg`. `extra` supplies any needed type declarations.
std::string holder(const std::string& container, const std::string& arg,
                   const std::string& extra = "") {
    return "module M; " + extra +
           " class H<T> "
           "  private owned f: " + container + " := new " + container + "(); "
           "  public method N(): int return f.Count; end N; "
           " end H; "
           " class C public static method Main(): int "
           "   using var h := new H<" + arg + ">(); "
           "   return h.N(); "
           " end Main; end C;";
}

// ── The issue's own repro, in the shape that makes it memory-unsafe ──────────
// Pre-fix this compiled and produced a heap-use-after-free (trace above).
const char* kUafRepro =
    "module M; "
    "class Holder<T> where T: class "
    "  private owned _l: OwningLinkedList<T> := new OwningLinkedList<T>(); "
    "  public method Put(owned x: T): int "
    "    _l.AddLast(owned x); "
    "    return _l.Count; "
    "  end Put; "
    "  public method Head(): T? "
    "    return _l.First; "
    "  end Head; "
    "end Holder; "
    "class C "
    "  public static method Round(i: int): int "
    "    using var h := new Holder<string>(); "
    "    var n := h.Put($\"value-{i}\"); "
    "    var s := h.Head(); "
    "    if s <> null then "
    "      return s!.Length; "
    "    end; "
    "    return 0; "
    "  end Round; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var j in 1..20 do "
    "      total := total + C.Round(j); "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

// ── ACCEPT control, run under ASan/LSan ─────────────────────────────────────
// The legitimate shape the fix must NOT disturb: the same generic holder at a
// proper class element type, actually exercising owning-element disposal (three
// `Res` objects per round, disposed by the list's own Dispose at scope end).
// runAotChecked catches both an over-correction (double free / SEGV) and a leak.
// Value: 7 rounds x 3 = 21.
const char* kClassElementDisposes =
    "module M; "
    "class Res public n: int := 0; end Res; "
    "class Holder<T> where T: class "
    "  private owned _l: OwningLinkedList<T> := new OwningLinkedList<T>(); "
    "  public method Put(owned x: T): int "
    "    _l.AddLast(owned x); "
    "    return _l.Count; "
    "  end Put; "
    "end Holder; "
    "class C "
    "  public static method Round(): int "
    "    using var h := new Holder<Res>(); "
    "    h.Put(new Res()); "
    "    h.Put(new Res()); "
    "    return h.Put(new Res()); "
    "  end Round; "
    "  public static method Main(): int "
    "    var total := 0; "
    "    for var i in 1..7 do "
    "      total := total + C.Round(); "
    "    end; "
    "    return total; "
    "  end Main; "
    "end C;";

}  // namespace

// =============================================================================
// REJECT — the per-instantiation sweep (§3.6 rule 2 / §14.4 / stdlib §4.2).
// Every case asserts the diagnostic TEXT, never a bare rejection.
// =============================================================================

// §14.4/§3.6 rule 2, v0.240 — the issue's repro: `string` reaching an owning
// collection through a generic parameter. This is the case that was
// memory-unsafe (heap-use-after-free); rejecting it is what closes the hazard.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     owning_linkedlist_string_via_generic_param_rejected_2713) {
    BEZEL_EXPECT_TRUE(hasPerInstantiationOwningError(kUafRepro, "`string`"));
}

// §14.4, v0.240 — the same hole with the simplest field spelling.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     owning_linkedlist_string_field_via_generic_param_rejected_2713) {
    BEZEL_EXPECT_TRUE(hasPerInstantiationOwningError(
        holder("OwningLinkedList<T>", "string"), "`string`"));
}

// §14.4, v0.240 — a primitive value type ("Value types are rejected at the
// instantiation site").
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     owning_array_int_via_generic_param_rejected_2713) {
    BEZEL_EXPECT_TRUE(hasPerInstantiationOwningError(
        holder("OwningArray<T>", "int"), "a primitive value type"));
}

// §14.4/§9, v0.240 — a record is a value type.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     owning_array_record_via_generic_param_rejected_2713) {
    BEZEL_EXPECT_TRUE(hasPerInstantiationOwningError(
        holder("OwningArray<T>", "Pt", "record Pt(X: int, Y: int); "),
        "a record (value type)"));
}

// §14.4/§10, v0.240 — an enum is a value type.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     owning_stack_enum_via_generic_param_rejected_2713) {
    BEZEL_EXPECT_TRUE(hasPerInstantiationOwningError(
        holder("OwningStack<T>", "Col", "enum Col A, B; end Col; "),
        "an enum (value type)"));
}

// §14.4/#1463, v0.240 — a value-type BUILTIN (declaration == null) reaches the
// same shared predicate through Pass 2, so #1463's fix carries across
// instantiation rather than being re-implemented (and re-skewed) there.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     owning_queue_guid_via_generic_param_rejected_2713) {
    BEZEL_EXPECT_TRUE(hasPerInstantiationOwningError(
        holder("OwningQueue<T>", "Guid"), "a value-type builtin"));
}

// §14.4, v0.240 — OwningSet's element slot.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     owning_set_string_via_generic_param_rejected_2713) {
    BEZEL_EXPECT_TRUE(hasPerInstantiationOwningError(
        holder("OwningSet<T>", "string"), "`string`"));
}

// §14.4/§28.11, v0.240 — for `OwningDictionary<K, V>` the OWNED slot is V
// (slot 1); the key is value-like by design. The sweep must follow slot 1.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     owning_dictionary_value_slot_via_generic_param_rejected_2713) {
    BEZEL_EXPECT_TRUE(hasPerInstantiationOwningError(
        "module M; "
        "class H<V> "
        "  private owned f: OwningDictionary<int, V> := "
        "      new OwningDictionary<int, V>(); "
        "  public method N(): int return f.Count; end N; "
        "end H; "
        "class C public static method Main(): int "
        "  using var h := new H<int>(); "
        "  return h.N(); "
        "end Main; end C;",
        "a primitive value type"));
}

// =============================================================================
// ACCEPT CONTROLS — a false reject is worse than the bug. Each of these would
// go red on an over-wide check.
// =============================================================================

// §14.4, v0.240 — a proper class element through the generic parameter.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     class_element_via_generic_param_admitted_2713) {
    BEZEL_EXPECT_TRUE(
        runs(0, holder("OwningLinkedList<T>", "Res",
                       "class Res public n: int := 0; end Res; ")));
}

// §14.4, v0.240 — "The same applies to interface types: `OwningArray<IConnection>`
// is admitted because `IConnection` may be implemented by class types with real
// disposal obligations."
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     interface_element_via_generic_param_admitted_2713) {
    BEZEL_EXPECT_TRUE(
        runs(0, holder("OwningArray<T>", "IRes",
                       "interface IRes method Ping(): int; end IRes; ")));
}

// §14.4, v0.240 — "Nullable class types and interfaces are admitted. The
// owning-type instantiation rule checks the *underlying* type, not the
// nullable-wrapper." (No `where T: class` here: that separate §14.2 constraint
// independently rejects `Res?`, which would mask what this control measures.)
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     nullable_class_element_via_generic_param_admitted_2713) {
    BEZEL_EXPECT_TRUE(
        runs(0, holder("OwningLinkedList<T>", "Res?",
                       "class Res public n: int := 0; end Res; ")));
}

// ⚠ §3.6 rule 2, v0.240 — THE SUBTLE ONE. Inside a generic body that is never
// instantiated, T is a placeholder (Kind::Named, declaration == nullptr), so a
// naive "is it a value type?" test answers "no" and lets it through while a
// naive "is it not a class?" test REJECTS the template itself. "A generic body
// type-checks under the weakest assumption its constraints establish" — the
// body alone is well-formed, and with no instantiation there is nothing to
// re-check. This is also what keeps the pinned control in
// owning_collection_value_builtin_1463_test.cpp:139-147 green.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     uninstantiated_generic_template_admitted_2713) {
    BEZEL_EXPECT_TRUE(runs(
        7,
        "module M; "
        "class NeverUsed<T> where T: class "
        "  private owned f: OwningSet<T> := new OwningSet<T>(); "
        "  public method N(): int return f.Count; end N; "
        "end NeverUsed; "
        "class C public static method Main(): int return 7; end Main; end C;"));
}

// §14.4, v0.240 — a NESTED owning collection: the element is itself an owning
// collection (a class-typed builtin with declaration == null). The predicate
// must not confuse "no TypeSymbol" with "value type".
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     nested_owning_collection_admitted_2713) {
    BEZEL_EXPECT_TRUE(
        runs(0, holder("OwningLinkedList<OwningArray<T>>", "Res",
                       "class Res public n: int := 0; end Res; ")));
}

// §14.4, v0.240 — TWO INDEPENDENT generics, each declaring its own owning
// collection, each instantiated concretely at a class.
//
// ⚠ This is deliberately NOT a forwarding test, and an earlier revision of this
// file named it as though it were: neither generic mentions the other, so
// nothing is ever re-keyed and it exercises only the direct path twice. The
// real forwarding shape — where the argument at an inner instantiation is
// ANOTHER generic's parameter — is the `chained_*` group below.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     two_independent_generics_at_class_elements_admitted_2713) {
    BEZEL_EXPECT_TRUE(runs(
        0,
        "module M; "
        "class Res public n: int := 0; end Res; "
        "class Inner<T> where T: class "
        "  private owned f: OwningArray<T> := new OwningArray<T>(); "
        "  public method N(): int return f.Count; end N; "
        "end Inner; "
        "class Outer<U> where U: class "
        "  private owned g: OwningArray<U> := new OwningArray<U>(); "
        "  public method N(): int return g.Count; end N; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var o := new Outer<Res>(); "
        "  using var i := new Inner<Res>(); "
        "  return o.N() + i.N(); "
        "end Main; end C;"));
}

// =============================================================================
// CHAINED (FORWARDED) PARAMETERS — the composed form of the same defect.
//
// `class Outer<U>` holds an `Inner<U>`, and it is INNER that declares the
// owning collection. The only instantiation of `Inner` is then `Inner<U>` —
// its argument unresolved — and NO `Inner<string>` node ever exists, so a
// single pass over the recording type's instantiations sees nothing concrete
// and the use-after-free returns at one extra level of composition. The sweep
// therefore re-keys the obligation to (enclosingType, that parameter) and
// iterates, the same closure checkDeferredNestedObligations runs for §14.2.
// =============================================================================

// §14.4/§3.6 rule 2, v0.240 — one level of forwarding. Verified ADMITTED with
// zero diagnostics before this closure existed.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     chained_forwarded_param_bound_to_string_rejected_2713) {
    BEZEL_EXPECT_TRUE(hasPerInstantiationOwningError(
        "module M; "
        "class Inner<T> where T: class "
        "  private owned _a: OwningArray<T> := new OwningArray<T>(); "
        "  public method N(): int return _a.Count; end N; "
        "end Inner; "
        "class Outer<U> where U: class "
        "  private owned g: Inner<U> := new Inner<U>(); "
        "  public method N(): int return g.N(); end N; "
        "end Outer; "
        "class C public static method Main(): int "
        "  using var o := new Outer<string>(); "
        "  return o.N(); "
        "end Main; end C;",
        "`string`"));
}

// §14.4/§3.6 rule 2, v0.240 — TWO levels of forwarding, so the closure must
// iterate rather than take a single re-key step.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     chained_two_level_forwarded_param_rejected_2713) {
    BEZEL_EXPECT_TRUE(hasPerInstantiationOwningError(
        "module M; "
        "class Inner2<T> where T: class "
        "  private owned _a: OwningArray<T> := new OwningArray<T>(); "
        "  public method N(): int return _a.Count; end N; "
        "end Inner2; "
        "class Mid<U> where U: class "
        "  private owned g: Inner2<U> := new Inner2<U>(); "
        "  public method N(): int return g.N(); end N; "
        "end Mid; "
        "class Top<V> where V: class "
        "  private owned h: Mid<V> := new Mid<V>(); "
        "  public method N(): int return h.N(); end N; "
        "end Top; "
        "class C public static method Main(): int "
        "  using var t := new Top<string>(); "
        "  return t.N(); "
        "end Main; end C;",
        "`string`"));
}

// ⚠ §14.4, v0.240 — the chain ACCEPT grid, all in one program so a single
// false reject anywhere fails the test: a one-level chain and a two-level
// chain both ending in a CLASS; a generic METHOD parameter shadowing the
// enclosing one at the intermediate link (`Shadow`'s own U is `string`, but
// the owning use is the METHOD's U — the chain must not attribute it to
// Shadow); and a RECURSIVE generic, which the visited set must terminate on
// rather than hang.
// ===========================================================================
// FIX #5393 (§14.1, v0.314): `Shadow<U> public method M<U>(): int` is now
// rejected at the declaration site. The whole grid no longer compiles.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     chained_forwarding_accept_grid_2713) {
    auto r = runAot(
        "module M; "
        "class Res public n: int := 0; end Res; "
        "class Inner<T> where T: class "
        "  private owned _a: OwningArray<T> := new OwningArray<T>(); "
        "  public method N(): int return _a.Count; end N; "
        "end Inner; "
        "class Outer<U> where U: class "
        "  private owned g: Inner<U> := new Inner<U>(); "
        "  public method N(): int return g.N(); end N; "
        "end Outer; "
        "class Outermost<V> where V: class "
        "  private owned h: Outer<V> := new Outer<V>(); "
        "  public method N(): int return h.N(); end N; "
        "end Outermost; "
        "class Shadow<U> where U: class "
        "  public method M<U>(): int "
        "    using var i := new Inner<U>(); "
        "    return i.N(); "
        "  end M; "
        "end Shadow; "
        "class Rec<T> where T: class "
        "  private owned _a: OwningArray<T> := new OwningArray<T>(); "
        "  public method Self(): int "
        "    using var r := new Rec<T>(); "
        "    return r.N(); "
        "  end Self; "
        "  public method N(): int return _a.Count; end N; "
        "end Rec; "
        "class C public static method Main(): int "
        "  using var a := new Outermost<Res>(); "
        "  using var b := new Shadow<string>(); "
        "  using var c := new Rec<Res>(); "
        "  return a.N() + b.M<Res>() + c.N() + 88; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);  // rejected at declaration site after fix #5393
}

// §28.11, v0.240 — an owning dictionary's KEY slot is deliberately value-like
// ("an owning dictionary owns its *values*; its keys are value-like"). A sweep
// that walked slot 0 instead of slot 1 would falsely reject this.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     owning_dictionary_value_like_key_admitted_2713) {
    BEZEL_EXPECT_TRUE(runs(
        0,
        "module M; "
        "class Res public n: int := 0; end Res; "
        "class H<V> where V: class "
        "  private owned f: OwningDictionary<string, V> := "
        "      new OwningDictionary<string, V>(); "
        "  public method N(): int return f.Count; end N; "
        "end H; "
        "class C public static method Main(): int "
        "  using var h := new H<Res>(); "
        "  return h.N(); "
        "end Main; end C;"));
}

// §14.4, v0.240 — "`OwningBuffer<T>`, `Span<T>`, and `ReadOnlySpan<T>` … are
// exceptions to this restriction. They admit T to be either class-typed … or
// value-typed (with plain value semantics)." A sweep that keyed on the
// `Owning` PREFIX rather than the enumerated collection family would falsely
// reject this.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     owning_buffer_value_element_admitted_2713) {
    BEZEL_EXPECT_TRUE(runs(
        4,
        "module M; "
        "class BufHolder<T> "
        "  private owned f: OwningBuffer<T>; "
        "  public constructor(owned b: OwningBuffer<T>) "
        "    f := owned b; "
        "  end constructor; "
        "  public method N(): int return f.Length; end N; "
        "end BufHolder; "
        "class C public static method Main(): int "
        "  using var h := new BufHolder<int>(new OwningBuffer<int>(4)); "
        "  return h.N(); "
        "end Main; end C;"));
}

// =============================================================================
// GENERIC-METHOD TYPE PARAMETERS — a method's parameter and the enclosing
// type's are ONE name scope, and a method parameter may SHADOW an enclosing
// one. Nothing forbids that: Appendix A.4's `GenericParam ::= Variance?
// Identifier` carries only the variance restriction, no §13/§14/§28.1 rule
// imposes name uniqueness, and neither sema nor the parser emits a shadowing
// diagnostic (not even for `M<T, T>`). The compiler already treats the two
// lists as a union — TypeResolution_Checks.cpp's `where`-clause checker
// concatenates them, commenting "both are in scope".
// =============================================================================

// ⚠ §3.6 rule 2 / §14.4, v0.240 — THE FALSE-REJECT GUARD. Inside `M2` the name
// `T` denotes the METHOD's parameter (bound to the class `Res`), not `Box`'s
// (bound to `string`). A sweep that keys the deferred obligation on the bare
// NAME plus the enclosing type validates the method's `T` against `Box`'s and
// rejects this legal program. Verified to do exactly that at f8210e9:
//   "`OwningArray` … this instantiation of `Box` binds type parameter `T` …
//    to `string`" — naming the wrong binding outright.
// ===========================================================================
// FIX #5393 (§14.1, v0.314): the declaration is now rejected at the type-
// parameter list. This test was pinning the accept; it now flips to rejection.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     method_param_shadowing_enclosing_param_admitted_2713) {
    auto r = runAot(
        "module M; "
        "class Res public n: int := 0; end Res; "
        "class Box<T> "
        "  public method M2<T>(): int "
        "    using var a := new OwningArray<T>(); "
        "    return a.Count; "
        "  end M2; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box<string>(); "   // Box's T = string
        "  return b.M2<Res>() + 3; "             // method's T = Res — legal
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);  // rejected at declaration site after fix #5393
}

// §14.4, v0.240 — the CONVERSE, so the guard above is not over-wide: the
// ENCLOSING type's parameter used inside a method body (including inside a
// generic method whose own parameter does NOT shadow it) is still recorded and
// still rejected. A guard that suppressed every in-method site would go green
// on the tests above and silently reopen the hole here.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     enclosing_param_used_inside_method_body_rejected_2713) {
    BEZEL_EXPECT_TRUE(hasPerInstantiationOwningError(
        "module M; "
        "class Box<T> "
        "  public method Make(): int "
        "    using var a := new OwningArray<T>(); "
        "    return a.Count; "
        "  end Make; "
        "  public method Make2<U>(): int "
        "    using var b := new OwningSet<T>(); "
        "    return b.Count; "
        "  end Make2; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var x := new Box<string>(); "
        "  return x.Make(); "
        "end Main; end C;",
        "`string`"));
}

// ⚠ KNOWN RESIDUAL, PINNED DELIBERATELY — this asserts what the compiler does
// TODAY, which is NOT what §14.4 requires. `OwningArray<U>` at `U = string` is
// ill-formed, but a generic METHOD's bindings are unreachable from this check:
// there is NO METHOD-LEVEL INSTANTIATION RECORD anywhere. `instantiations_` has
// exactly one push site (TypeResolution_Names.cpp), keyed `inst.type = tsym` —
// a TypeSymbol — so only generic TYPE instantiations are recorded. (It is NOT a
// pass-ordering problem: a call's EXPLICIT method type arguments are resolved
// inside `resolveTypes` too, at Names.cpp's MemberAccessExpr::typeArgs and
// IdentifierExpr::typeArgs arms. What is missing is the association back to the
// method.) Closing this needs a new record, and is filed as #2736(a).
// Recording nothing here is the deliberate choice: the
// alternative (keying on the bare name) is precisely the false reject the test
// above guards. When #2736(a) lands, THIS TEST MUST FLIP to a rejection.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     method_own_param_bound_to_string_still_admitted_pending_2736a) {
    BEZEL_EXPECT_TRUE(runs(
        9,
        "module M; "
        "class Box2<T> "
        "  public method M3<U>(): int "
        "    using var a := new OwningArray<U>(); "
        "    return a.Count; "
        "  end M3; "
        "end Box2; "
        "class Res public n: int := 0; end Res; "
        "class C public static method Main(): int "
        "  using var x := new Box2<Res>(); "
        "  return x.M3<string>() + 9; "
        "end Main; end C;"));
}

// =============================================================================
// MEMORY-SAFETY CONTROL — the legitimate owning path stays sanitizer-clean.
// =============================================================================

// §14.4/§28.11, v0.240 — the generic owning holder at a CLASS element type
// compiles, links, runs, and disposes its elements exactly once under ASan/LSan.
// This is the over-correction guard: a fix that broke the real owning path (or
// that started double-disposing) fails here rather than silently.
// Value: 7 rounds x 3 elements = 21.
BEZEL_TEST(sema_owning_element_generic_inst_2713,
     class_element_generic_holder_runs_clean_2713) {
    JitResult r = runAotChecked(kClassElementDisposes);
    if (wasSkipped(r)) return;  // ASan toolchain unavailable — skip, not a fail
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 21);
}
