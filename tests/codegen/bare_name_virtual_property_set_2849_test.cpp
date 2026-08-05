// Spec-derived regression test for issue #2849.
//
// §12.1 (Members are non-virtual by default) / §6.6 (Compound assignments) /
// §23.3 (Block-form properties) / §12.3 (`base.Member`) / §22.12 (Static
// members), v0.240.
//
// §12.1 states, verbatim:
//
//     "A method, property, or indexer is non-virtual unless marked `virtual`.
//      To override a virtual member in a derived class, mark the matching
//      member `override`."
//
// and, in the same section:
//
//     "Fields, constructors, destructors, and static members may not be
//      virtual."
//
// THE DEFECT. A write to one's own `virtual`/`override` property spelled
// WITHOUT a receiver — the bare `P := v` / `P OP= v` form, which is the
// idiomatic spelling — bypassed vtable dispatch and ran the COMPILE-TIME type's
// setter. `Emitter::lowerVirtualPropertySet` (src/codegen/LlvmBackend_Exprs.cpp)
// opened with
//
//     auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target);
//     if (!ma || !ma->target) return false;
//
// so a bare `P` target — an `ast::IdentifierExpr` — declined, and every caller
// fell through to the statically bound setter. Its GETTER twin
// `lowerVirtualPropertyGet`, thirty lines above in the same file, deliberately
// handles both spellings. The asymmetry between the twins was the whole bug.
//
// MEASURED on b0fa41a with `B`'s accessors at +100/+1 and `D`'s at +200/+2, the
// receiver always a `D`:
//
//     spelling        measured   correct
//     P (bare read)      200       200     ok — the getter twin dispatches
//     this.P (read)      200       200     ok
//     d.P (read)         200       200     ok
//     P := 7               8         9     WRONG — B's setter
//     this.P := 7          9         9     ok
//     P += 1             202       203     WRONG — virtual get, static set
//     this.P += 1        203       203     ok
//     b.P += 1           203       203     ok
//
// 202 is the diagnostic value: the getter DISPATCHED (D's 200, +1 = 201) and the
// setter did NOT (B's `value + 1` = 202) — the two halves of one statement
// disagreeing. Two further shapes were not merely wrong but DROPPED ENTIRELY
// (measured 0, no diagnostic), because for them the fall-through static path has
// no setter body to inline either: an `abstract` read-write property written
// through the bare name inside the abstract base's own concrete method, and a
// virtual AUTO-property (T12/T13 below).
//
// THE FIX gives `lowerVirtualPropertySet` the same two-spelling head its getter
// twin has: a bare `IdentifierExpr` target whose symbol is a non-indexer
// `PropertySymbol` uses `currentThis_` as the receiver. `base.P` keeps
// declining per §12.3, and a STATIC property still cannot reach the dispatch —
// §12.1 forbids a static member from being virtual, so buildVtable never
// assigns it a slot and the `propSetSlot_` lookup declines it (T10).
//
// Every case asserts a CORRECT VALUE, not a clean compile: the defect compiled
// cleanly and produced the wrong number (or nothing at all). Constants are all
// < 256 (runAot's exit status is WEXITSTATUS-masked to 8 bits).
//
// EXPLICITLY OUT OF SCOPE, measured and filed rather than folded in:
//   * `P++` is a THIRD lowering path — `lowerStmt`'s `ast::IncrementStmt` arm,
//     which reads through `computedSetterOf` and writes through
//     `lowerPropertySetValue` — and this fix does not reach it. MEASURED on
//     b0fa41a and again with this fix applied: `P++`, `this.P++` and `b.P++`
//     all give 202 where 203 is correct, unchanged by this commit; an
//     `abstract` and a virtual AUTO property are DROPPED there (0). Filed
//     separately (#2858).
//   * a virtual property whose declared type is NULLABLE (`property Q: int?`)
//     does not compile at all, on ANY spelling: `this.Q := 7` and `d.Q := 7`
//     fail IR verification ("Call parameter type does not match function
//     signature ... { i8, i32 } call void %3(ptr %0, i32 7)") because this
//     function coerces the value with `coerceNumericToType` and never wraps it
//     into the nullable-value struct the setter slot expects. Pre-fix the bare
//     spelling ABORTED bezelc outright (an LLVM `checkGEPType` assertion, core
//     dumped); post-fix it joins its two siblings at the IR-verification
//     diagnostic — strictly better, and no working program changes. Filed
//     separately (#2859); deliberately not papered over here.
//
// Related: #481 (the vtable property dispatch this completes), #2836 (the
// indexer-compound twin, whose scope note prompted the measurement and which
// this refutes as stated — `this.P += 1` and `b.P += 1` were already correct),
// #2848 (the same IncrementStmt arm's INDEXER branch), #2838.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAot;
using bezel::test::runAotOut;

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

// The shared virtual/override pair. `B`'s accessors are +100/+1, `D`'s are
// +200/+2, so a bare `P := 7` on a `D` yields
//   * 9   when the setter dispatches to D (correct),
//   * 8   when it binds statically to B (the defect),
//   * 0   when the statement is dropped entirely,
// and a bare `P += 1` yields 203 / 202 / 0 the same way — six values that
// cannot be confused for one another.
constexpr const char* kPair =
    "class B "
    "  public _s: int := 0; "
    "  public virtual property P: int "
    "    get => _s + 100; "
    "    set => do _s := value + 1; end; "
    "  end; "
    "  public method SetBare(): int "
    "    P := 7; "
    "    return _s; "
    "  end SetBare; "
    "  public method CompBare(): int "
    "    P += 1; "
    "    return _s; "
    "  end CompBare; "
    "end B; "
    "class D: B "
    "  public override property P: int "
    "    get => _s + 200; "
    "    set => do _s := value + 2; end; "
    "  end; "
    "end D; ";

}  // namespace

// ---------------------------------------------------------------------------
// T1 — the issue's own repro, the PLAIN bare write. §12.1. Pre-fix: 8.
// Post-fix: 9.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           bare_plain_set_dispatches_override_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); return d.SetBare(); "
        "end Main; end C;")), 9);
}

// ---------------------------------------------------------------------------
// T2 — the bare COMPOUND write. §6.6, verbatim: "when the final segment is
// accessor-mediated (a property or an indexer), its getter and setter are each
// invoked exactly once — the getter produces the left operand, the setter
// stores the result". Pre-fix the two halves disagreed: 202. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           bare_compound_set_dispatches_override_6_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); return d.CompBare(); "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T3 — the EXPLICIT-RECEIVER spellings, which were already correct. They are
// the reason the defect reads as an inconsistency rather than a missing
// feature, and they must stay correct. (They also refute #2836's scope note,
// which had inferred — explicitly as inferred — that `lowerPropertyCompound`
// carried #2836's hole; it does not.)
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           explicit_receiver_spellings_stay_correct_12_1) {
    const std::string decls =
        std::string("module M; ") +
        "class B "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method SetThis(): int   this.P := 7;   return _s; end SetThis; "
        "  public method CompThis(): int  this.P += 1;   return _s; end CompThis; "
        "end B; "
        "class D: B "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int ";
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "using var d := new D(); return d.SetThis(); end Main; end C;")),
        9);
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "using var d := new D(); return d.CompThis(); end Main; end C;")),
        203);
    // Through a BASE-typed reference from outside the class.
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "using var d := new D(); var b: B := d; b.P += 1; return d._s; "
                "end Main; end C;")), 203);
    // The bare READ, which the getter twin already dispatched.
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") + kPair +
        "class C public static method Main(): int "
        "  using var d := new D(); return d.P; end Main; end C;")), 200);
}

// ---------------------------------------------------------------------------
// T4 — inside a LAMBDA body (§27.3). `currentThis_` there is the captured
// receiver, so the new bare-name branch must resolve it exactly as the getter
// twin already does. Pre-fix: 202 (compound) / 8 (plain). Post-fix: 203 / 9.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           bare_write_inside_lambda_body_27_3) {
    const std::string decls =
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method CompLambda(): int "
        "    var f := lambda(n: int) => do P += n; end; "
        "    f(1); "
        "    return _s; "
        "  end CompLambda; "
        "  public method SetLambda(): int "
        "    var g := lambda(n: int) => do P := n; end; "
        "    g(7); "
        "    return _s; "
        "  end SetLambda; "
        "end B; "
        "class D: B "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int ";
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "using var d := new D(); return d.CompLambda(); "
                "end Main; end C;")), 203);
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "using var d := new D(); return d.SetLambda(); "
                "end Main; end C;")), 9);
}

// ---------------------------------------------------------------------------
// T5 — inside an ASYNC body (§30.1): a heap state-machine frame, not a closure.
// Pre-fix: 202. Post-fix: 203.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           bare_write_inside_async_body_30_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public async method Bump(): Task<int> "
        "    P += 1; "
        "    return _s; "
        "  end Bump; "
        "end B; "
        "class D: B "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.Bump().Result; "
        "end Main; end C;")), 203);
}

// ---------------------------------------------------------------------------
// T6 — an ABSTRACT read-write property, written through the bare name inside
// the abstract base's own concrete method. §12.5: "`abstract` on a member
// declares it without a body". There is no base accessor body to inline, so
// pre-fix BOTH spellings were DROPPED ENTIRELY: 0, with no diagnostic.
// Post-fix: 9 (plain) and 203 (compound).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           abstract_property_through_bare_name_12_5) {
    const std::string decls =
        "module M; "
        "abstract class A "
        "  public _s: int := 0; "
        "  public abstract property P: int get; set; end; "
        "  public method SetBare(): int   P := 7;  return _s; end SetBare; "
        "  public method CompBare(): int  P += 1;  return _s; end CompBare; "
        "end A; "
        "class D: A "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int ";
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "using var d := new D(); return d.SetBare(); end Main; end C;")),
        9);
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "using var d := new D(); return d.CompBare(); end Main; end C;")),
        203);
}

// ---------------------------------------------------------------------------
// T7 — a three-level chain B -> D -> E: the bare write must reach the
// MOST-DERIVED override, not merely "one level down". E's getter is +300 and
// its setter +3, so `P += 1` inside B's method on an `E` is 300 -> 301 -> 304.
// Pre-fix: 302 (E's getter dispatched, B's setter did not) — the clearest
// single demonstration that only the setter half was static.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           most_derived_override_in_a_chain_12_1) {
    const std::string prog =
        std::string("module M; ") + kPair +
        "class E: D "
        "  public override property P: int "
        "    get => _s + 300; "
        "    set => do _s := value + 3; end; "
        "  end; "
        "end E; "
        "class C public static method Main(): int ";
    // > 255, so the observable moves to stdout.
    BEZEL_EXPECT_TRUE(runAotOut(
        prog + "using var e := new E(); Console.WriteLine(e.CompBare()); "
               "return 0; end Main; end C;") == "304");
    BEZEL_EXPECT_EQ(run(runAot(
        prog + "using var d := new D(); return d.CompBare(); end Main; end C;")),
        203);
    BEZEL_EXPECT_EQ(run(runAot(
        prog + "using var b := new B(); return b.CompBare(); end Main; end C;")),
        102);
}

// ---------------------------------------------------------------------------
// T8 — a `sealed override` (§12.4: "`sealed` on an override prevents further
// overriding"). Sealing ends the chain but does NOT remove the slot, so a bare
// write from the BASE's method must still dispatch. Pre-fix: 8. Post-fix: 9.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           sealed_override_still_dispatches_12_4) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method SetBare(): int  P := 7;  return _s; end SetBare; "
        "end B; "
        "class D: B "
        "  public sealed override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.SetBare(); "
        "end Main; end C;")), 9);
}

// ---------------------------------------------------------------------------
// T9 — a GENERIC declaring class: the value type is read off `pd->type` and
// must lower against the active substitution in the specialization.
// Pre-fix: 8. Post-fix: 9.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           generic_declaring_class_dispatches_14_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B<T> "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method SetBare(): int  P := 7;  return _s; end SetBare; "
        "end B; "
        "class D<T>: B<T> "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D<int>(); return d.SetBare(); "
        "end Main; end C;")), 9);
}

// ---------------------------------------------------------------------------
// ACCEPT CONTROLS — shapes the fix must NOT change. A false dispatch is worse
// than the bug it fixes: these are the ways this change could break working
// code. Every one is asserted at the SAME value before and after.
// ---------------------------------------------------------------------------

// T10 — a STATIC property. §12.1, verbatim: "Fields, constructors, destructors,
// and static members may not be virtual", so it has no vtable slot and a bare
// `SP := 7` inside a static method must keep running its own setter (7 + 1 = 8).
// This is the case that would break if the new branch used `currentThis_`
// without the slot lookup gating it. 8 before and after.
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           static_property_bare_write_unchanged_22_12) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class H "
        "  public static _q: int := 0; "
        "  public static property SP: int "
        "    get => _q + 100; "
        "    set => do _q := value + 1; end; "
        "  end; "
        "  public static method SetBare(): int  SP := 7;  return _q; end SetBare; "
        "end H; "
        "class C public static method Main(): int "
        "  return H.SetBare(); "
        "end Main; end C;")), 8);
}

// T11 — `base.P := v` / `base.P OP= v` must NOT dispatch. §12.3, verbatim:
// "Inside a derived member, `base.Name(...)` calls the base class's
// implementation, bypassing virtual dispatch." The base property shares its
// vtable slot with the derived override, so a dispatched `base.P` would run the
// override. 8 and 102 before and after.
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           base_qualified_write_stays_non_virtual_12_3) {
    const std::string decls =
        "module M; "
        "class B "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "end B; "
        "class D: B "
        "  public override property P: int "
        "    get => _s + 200; "
        "    set => do _s := value + 2; end; "
        "  end; "
        "  public method SetBase(): int   base.P := 7;  return _s; end SetBase; "
        "  public method CompBase(): int  base.P += 1;  return _s; end CompBase; "
        "end D; "
        "class C public static method Main(): int ";
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "using var d := new D(); return d.SetBase(); end Main; end C;")),
        8);
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "using var d := new D(); return d.CompBase(); end Main; end C;")),
        102);
}

// A NON-VIRTUAL property stays statically bound (§12.1: "non-virtual unless
// marked `virtual`"). It has no slot, so the new branch declines. 8 and 102
// before and after.
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           non_virtual_property_stays_static_12_1) {
    const std::string decls =
        "module M; "
        "class N "
        "  public _s: int := 0; "
        "  public property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method SetBare(): int   P := 7;  return _s; end SetBare; "
        "  public method CompBare(): int  P += 1;  return _s; end CompBare; "
        "end N; "
        "class C public static method Main(): int ";
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "using var n := new N(); return n.SetBare(); end Main; end C;")),
        8);
    BEZEL_EXPECT_EQ(run(runAot(
        decls + "using var n := new N(); return n.CompBare(); end Main; end C;")),
        102);
}

// A `virtual` property the derived class does NOT override: the slot holds the
// base's own accessors, so dispatch and static binding must agree at 8. This is
// the shape a naive "if there is a slot, run the derived body" fix would break.
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           virtual_without_override_reaches_base_body_12_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        std::string("module M; ") +
        "class B "
        "  public _s: int := 0; "
        "  public virtual property P: int "
        "    get => _s + 100; "
        "    set => do _s := value + 1; end; "
        "  end; "
        "  public method SetBare(): int  P := 7;  return _s; end SetBare; "
        "end B; "
        "class D: B end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.SetBare(); "
        "end Main; end C;")), 8);
}

// ---------------------------------------------------------------------------
// VALUE-KIND AXES
// ---------------------------------------------------------------------------

// T12 — a virtual AUTO-property (§23.1's `property A: int := 0;` form). The
// override has its own backing field, and pre-fix the bare write was DROPPED
// ENTIRELY (measured 0): the fall-through static path stores into the BASE's
// backing field while the bare read goes through the dispatched getter, so the
// value written was never observed. Post-fix: 7.
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           virtual_auto_property_bare_write_23_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class B "
        "  public virtual property A: int := 0; "
        "  public method SetBare(): int  A := 7;  return A; end SetBare; "
        "end B; "
        "class D: B "
        "  public override property A: int := 0; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); return d.SetBare(); "
        "end Main; end C;")), 7);
}

// T13 — a `string`-typed property. The integer cases cannot observe the
// refcount discipline; this one runs the concatenating desugaring through the
// dispatched setter and pins the VALUE. D's getter appends "g", its setter
// appends "s": "b" -> "bg" -> "bgx" -> stored "bgxs" (pre-fix "bgxS", i.e. the
// dispatched getter with B's setter). The named-local plain set is the second
// half, because the two take different release paths inside the setter.
//
// MEMORY (measured separately with LSan on, 5 iterations of each spelling):
// the bare form leaks 215 B in 10 allocations from the dispatched GETTER —
// BYTE-IDENTICAL to what the already-correct `this.S += ...` spelling leaks on
// the same program, so this fix adds no leak of its own; it makes the bare
// spelling behave exactly like its explicit-receiver twin, pre-existing leak
// included. No use-after-free and no double free on either spelling.
BEZEL_TEST(codegen_bare_virtual_property_set_2849,
           string_property_bare_write_dispatches_4_6) {
    const std::string decls =
        "module M; "
        "class B "
        "  public _s: string := \"b\"; "
        "  public virtual property S: string "
        "    get => _s + \"G\"; "
        "    set => do _s := value + \"S\"; end; "
        "  end; "
        "  public method Comp(): string   S += \"x\";  return _s; end Comp; "
        "  public method Set(t: string): string  S := t;  return _s; end Set; "
        "end B; "
        "class D: B "
        "  public override property S: string "
        "    get => _s + \"g\"; "
        "    set => do _s := value + \"s\"; end; "
        "  end; "
        "end D; "
        "class C public static method Main(): int ";
    auto comp = runAot(
        decls + "using var d := new D(); Console.WriteLine(d.Comp()); return 0; "
                "end Main; end C;");
    BEZEL_EXPECT_TRUE(comp.ran);
    BEZEL_EXPECT_TRUE(comp.stdoutText.find("bgxs") != std::string::npos);
    // The statically-bound setter's answer, which must NOT appear.
    BEZEL_EXPECT_TRUE(comp.stdoutText.find("bgxS") == std::string::npos);
    auto set = runAot(
        decls + "using var d := new D(); var lv := \"zz\"; "
                "Console.WriteLine(d.Set(lv)); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(set.ran);
    BEZEL_EXPECT_TRUE(set.stdoutText.find("zzs") != std::string::npos);
    BEZEL_EXPECT_TRUE(set.stdoutText.find("zzS") == std::string::npos);
}

