// Codegen — issue #2469: `val.ToString()` on a bare generic type parameter
// `T` (`where T: value`) substituted with a RECORD or ENUM segfaulted; the
// PRIMITIVE substitution already worked (#515/#608's dedicated fallback in
// `Emitter::lowerCall`'s `if (!ms)` block, LlvmBackend_Calls2.cpp).
//
// Root cause: `tryLowerValueSemanticsCall`'s #1091 block resolves the bare
// type-parameter's concrete substitution (via `genericSubstSemaTy_`) and
// routes GetHashCode/Equals through `lowerValueSemMethodOn`, but the same
// commit deliberately left ToString OUT of that redirect, reasoning "ToString
// already works via the genericSubst path in the ms==null fallback". That
// separate fallback (the `ma->member == "ToString"` case inside `lowerCall`'s
// `if (!ms)` block) has a branch for a PRIMITIVE concrete substitution
// (`emitPrimitiveToString`) and for a CLASS/INTERFACE concrete substitution
// (`lowerObjectToString`, the uniform vtable slot) — but NO branch for a
// RECORD or ENUM concrete substitution. `val.ToString()` at T = a record (or
// enum) therefore fell all the way through every check in that large block to
// `lowerCall`'s own unconditional `return nullptr;`, producing `ret ptr null`
// for the `string`-typed return. The caller's subsequent use of the result
// (e.g. `s.Length`) then dereferenced a near-null pointer — reproduced with
// AddressSanitizer as `SEGV on unknown address 0x000000000008` (a read at
// offset 8 from a null base) before this fix.
//
// The fix adds a narrowly-scoped block to `tryLowerValueSemanticsCall`
// (LlvmBackend_Calls2.cpp, immediately after the #1091 block): when the bare
// type-parameter's concrete substitution is specifically a Record or Enum,
// reuse `lowerValueSemMethodOn` — the SAME dispatcher a directly (non-
// generic) typed record/enum receiver's `.ToString()` already uses
// unconditionally (`namedValueSem` in the same function, no member filter),
// per language spec §8.5. Primitive and Class/Interface substitutions are
// deliberately excluded from this new block (they keep using their existing,
// working, separately-tracked paths) — see the fix's own comment for why
// widening the shared #1091 gate instead would have introduced a double-
// receiver-lowering hazard (#1075) and undefined behavior (`call.args[0]` on
// a zero-arg ToString call in the `concreteRefSem` arm).
//
// Every VALUE below is asserted against the exact §8.5 default-formatting
// rule for its shape, not merely "did not crash":
//   - "For positional records: a representation of fields, e.g.
//     `"Point(X: 10, Y: 20)"`."
//   - "For body records that satisfy the auto-equality conditions … a
//     representation of auto-properties, formatted as
//     `"TypeName(Prop1: value1, Prop2: value2)"`."
//   - "For body records that do not satisfy auto-equality conditions, and
//     for enums: the type's qualified name."

#include "support/aot_support.hpp"   // runAot / runAotOut — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── The exact #2469 repro: positional record substitution ──────────────────

// `F<T>(val: T): string where T: value { return val.ToString(); }` called
// with `new Pt(3, 4)` (a positional record). Before the fix: SIGSEGV at
// runtime (confirmed via ASan: SEGV read at offset 0x8 from a null pointer).
// After the fix: the §8.5 positional-record default, value-asserted via
// stdout (not just "did not crash").
BEZEL_TEST(generic_bare_typeparam_tostring_record_2469,
           positional_record_substitution_value_correct) {
    const std::string program =
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(new Pt(3, 4)); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runAotOut(program), "Pt(X: 3, Y: 4)");
}

// The same repro via exit code, pinning that the returned string is USABLE
// by the caller (`.Length`), not merely non-null — the exact shape of the
// original #2469 filing's own repro (`s.Length > 0`).
BEZEL_TEST(generic_bare_typeparam_tostring_record_2469,
           positional_record_substitution_length_usable) {
    const std::string program =
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(new Pt(3, 4)); "
        "    return (if s.Length = 14 then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Body record, auto-hashable (§9.2 / §8.5) ────────────────────────

// An eligible body record (only auto-properties + a property-initializing
// constructor) substituted for T: the §9.2/§8.5 auto-ToString format applies
// identically to the generic-substituted receiver as it does to a directly
// (non-generic) typed one.
BEZEL_TEST(generic_bare_typeparam_tostring_record_2469,
           body_record_auto_equality_substitution_value_correct) {
    const std::string program =
        "module M; "
        "record Pt "
        "  public property X: int; "
        "  public property Y: int; "
        "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
        "end Pt; "
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(new Pt(7, 8)); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runAotOut(program), "Pt(X: 7, Y: 8)");
}

// A body record with an explicit field is NOT auto-hashable (§9.2:
// "A body record is not auto-hashable … when it contains: an
// explicit field declaration"), so its §8.5 ToString default is the type's
// qualified name, not the property-listing form — asserted through the SAME
// generic-substitution path to pin that `lowerValueSemMethodOn`'s existing
// `!autoBearing` branch (LlvmBackend_Calls2.cpp) is reached correctly here
// too, not just the auto-hashable one above.
BEZEL_TEST(generic_bare_typeparam_tostring_record_2469,
           body_record_non_auto_equality_substitution_type_name_default) {
    const std::string program =
        "module M; "
        "record Opaque "
        "  public _tag: int; "
        "  public constructor(t: int) _tag := t; end constructor; "
        "end Opaque; "
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(new Opaque(5)); "
        // #2934 (§8.5, v0.252): was "Opaque" — a body record with an
        // explicit field is not auto-hashable (§9.2), so its default is
        // the QUALIFIED type name, through the bare-T substitution too.
        "    return (if s = \"M.Opaque\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Enum substitution (§8.5: enums default to the qualified type name) ─────

// An enum with no user ToString override, substituted for T: the §8.5 enum
// default (the type's qualified name). Enums are admitted by `where T:
// value` per §14.2.
BEZEL_TEST(generic_bare_typeparam_tostring_record_2469,
           enum_substitution_type_name_default_value_correct) {
    const std::string program =
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(Color.Green); "
        // #2934 (§8.5, v0.252): was "Color".
        "    return (if s = \"M.Color\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// ── Regression guards: the paths this fix must NOT disturb ─────────────────

// A PRIMITIVE substitution (T = int) already worked via #515/#608's dedicated
// `emitPrimitiveToString` fallback — this fix adds a narrowly-scoped Record/
// Enum-only block ahead of it and must not intercept the primitive case.
BEZEL_TEST(generic_bare_typeparam_tostring_record_2469,
           regression_primitive_substitution_still_correct) {
    const std::string program =
        "module M; "
        "class C "
        "  public static method F<T>(val: T): string where T: value "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    var s := F(7); "
        "    return (if s = \"7\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// A CLASS-constrained type parameter (`where T: SomeClass`) dispatches
// ToString through the uniform vtable slot (#608's `isBuiltinExRecv`/
// `isNamedClassRecv`-adjacent path is not this fix's concern, but the
// generic-substitution `lowerObjectToString` branch in the SAME `if (!ms)`
// fallback is) — this fix's new block is gated to Record/Enum only and must
// not affect a class substitution's existing dispatch.
BEZEL_TEST(generic_bare_typeparam_tostring_record_2469,
           regression_class_substitution_still_correct) {
    const std::string program =
        "module M; "
        "class Widget "
        "  public override method ToString(): string return \"a-widget\"; end ToString; "
        "  destructor end destructor; "
        "end Widget; "
        "class C "
        "  public static method F<T>(val: T): string where T: Widget "
        "    return val.ToString(); "
        "  end F; "
        "  public static method Main(): int "
        "    using var w := new Widget(); "
        "    var s := F(w); "
        "    return (if s = \"a-widget\" then 42 else 1); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// fix #5449 (§3.6 rule 2 / §14.2, v0.315) BEHAVIOUR FLIP: the
// canary_generic_record_field_access_unaffected test is RETRACTED.
// Per the owner ruling on #5449: "A bare type parameter's member surface
// is what its constraints establish, never the concrete substitution's."
// `_val.X` on an unconstrained `T` is a compile error — the member is
// not in T's constraint-derived surface (T has no constraints). The
// codegen permissive backstop is no longer reached for this shape; the
// new sema check emits a declaration-site error naming T and the missing
// member, with the remedy of adding a constraint that declares it.
// OLD EXPECTATION (accept): run(program) == 9
// NEW EXPECTATION (reject): the sema diagnostic fires.
// The working twin that preserves the covered behaviour (field access on
// T where T has an appropriate constraint) follows below.
BEZEL_TEST(generic_bare_typeparam_tostring_record_2469,
           canary_generic_record_field_access_unaffected) {
    const std::string program =
        "module M; "
        "record Pt(X: int, Y: int); "
        "class Container<T> "
        "  protected _val: T; "
        "  public constructor(v: T) _val := v; end constructor; "
        "  public method GX(): int return _val.X; end GX; "
        "  destructor end destructor; "
        "end Container; "
        "class C public static method Main(): int "
        "  using var c := new Container<Pt>(new Pt(9, 5)); "
        "  return c.GX(); "
        "end Main; end C;";
    // #5449: the sema check fires at the declaration site (§3.6 rule 2).
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);          // must not have run
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors()); // and must have an error
    // The error names bare type parameter T and the missing member X.
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("bare type parameter") != std::string::npos &&
            d.message.find("T") != std::string::npos &&
            d.message.find("X") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);  // names T and X with the bare-type-parameter wording
}

// WORKING TWIN: the same field access works when T is constrained by an
// interface that declares the member. This preserves the covered behaviour
// (generic-record field access) under the spec-compliant constraint model.
// Per the #5449 ruling: "add a constraint that declares it" is the remedy.
BEZEL_TEST(generic_bare_typeparam_tostring_record_2469,
           with_interface_constraint_member_access_works) {
    // The fix #5449 adds: add a `where T: <constraint>` that declares X.
    // Container<T> is now constrained to T: IHasX, so _val.X resolves.
    const std::string program =
        "module M; "
        "interface IHasX\n"
        "  property X: int;\n"
        "end IHasX;\n"
        "class Pt: IHasX\n"
        "  public _x: int;\n"
        "  public _y: int;\n"
        "  public constructor(x: int, y: int) _x := x; _y := y; end constructor;\n"
        "  public property X: int\n"
        "    get => _x;\n"
        "    set => _x := value;\n"
        "  end X;\n"
        "  destructor end destructor;\n"
        "end Pt;\n"
        "class Container<T> where T: IHasX\n"
        "  protected _val: T;\n"
        "  public constructor(v: T) _val := v; end constructor;\n"
        "  public method GX(): int return _val.X; end GX;\n"
        "  destructor end destructor;\n"
        "end Container;\n"
        "class C public static method Main(): int\n"
        "  using var c := new Container<Pt>(new Pt(9, 5));\n"
        "  return c.GX();\n"
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 9);
}
