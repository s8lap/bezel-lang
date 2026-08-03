// Spec-derived tests for §17.6 (v0.236, issue #2585): the increment and
// decrement operators `++`/`--` "apply only when the reference type
// overloads the operator" on REFERENCE-type operands (§4.6: string,
// classes, interfaces, function types, and the heap-array builtins are
// reference types). A reference type that does NOT declare a matching
// `operator ++`/`operator --` overload (per §25) must be rejected at the
// front end.
//
// Pre-fix (#2585): the compiler silently accepted `s.P++` on a
// string-typed computed property (and `s++` on a string local, `k++` on a
// class-typed local, etc.) and codegen then built
// `ConstantInt::get(cur->getType() = ptr, 1)` — a degenerate constant —
// and fed it to `emitCompoundOp` -> `emitCheckedArith(sadd.with.overflow,
// ptr, i0)`, producing malformed IR (`Call parameter type does not match
// function signature! ... @llvm.sadd.with.overflow.p0(ptr %9, i0 0)`)
// that `verifyModule` rejects. Three codegen sites in the IncrementStmt
// handler (LlvmBackend_Stmts.cpp) carried the same shape: the
// local/field "Built-in numeric" branch (~line 1273), the block-setter
// computed-property branch (~line 1311), and the user-defined-indexer
// branch (~line 1473).
//
// The fix lives in ExprTypecheck.cpp's IncrementStmt handler: after the
// target's type is resolved, if the type is a reference type (§4.6,
// nullability ignored) AND the type does not declare a user-defined
// `operator ++`/`operator --` overload (mirroring codegen's own overload
// lookup at LlvmBackend_Stmts.cpp ~line 1158), a §17.6 diagnostic is
// reported and codegen never runs on the operand. Value-type numeric
// primitives keep their built-in `++` (§17.6: "For built-in numeric
// types, `x++` is equivalent to `x := x + 1`"), and a record/class that
// DOES overload `++` still resolves through the overload (the §17.6
// `Counter` example, §25.3) — both proven by the ACCEPT controls below.
//
// Tests (REJECT = the bug; ACCEPT = no-false-reject control):
//   REJECT shapes (§17.6 violation — was an ICE pre-fix):
//    1. `c.P++` on a string-typed computed property (the issue's repro)
//    2. `s++` on a string local
//    3. `k++` on a class-typed local whose class does NOT overload ++
//    4. `s--` on a string local (the `--` twin)
//    5. `o.P--` on an Object-typed computed property (non-string primitive
//       reference type — proves the check is not string-only)
//   ACCEPT shapes (no false reject — a regression here is worse than #2585):
//    6. `i++` on an int local
//    7. `l++` on a long local
//    8. `d++` on a double local
//    9. `ch++` on a char local
//   10. `arr[0]++` on an inline-array int element
//   11. `c++` on a record that overloads `operator ++` (the §17.6 / §25.3
//       `Counter` shape — proves the overload-lookup path still admits a
//       reference... record value type with an overload)
//   12. `c++` on a CLASS that overloads `operator ++` (the literal §17.6
//       reference-type-with-overload admit — the primary no-false-reject
//       axis for the reference-type branch)

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// =========================================================================
// §17.6 / #2585 — REJECTION SHAPES (pre-fix: codegen ICE / malformed IR)
// =========================================================================

// §17.6 / #2585 primary repro: `c.P++` on a string-typed computed property
// (block-setter form). Pre-fix: codegen's block-setter branch built
// `ConstantInt::get(ptr, 1)` -> `@llvm.sadd.with.overflow.p0(ptr, i0 0)`
// -> verifyModule rejected the IR. Post-fix: clean sema REJECT citing §17.6.
BEZEL_TEST(increment_reference_type_2585,
           string_computed_property_increment_rejected_spec_17_6_issue_2585) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _x: string := \"hi\"; "
        "  public property P: string "
        "    get => _x; "
        "    set => do _x := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    c.P++; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "`++` is not defined on reference type `string`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§17.6"));
}

// §17.6 / #2585: `s++` on a string LOCAL — same defect on the
// local/field codegen branch (~line 1273). Pre-fix ICE identical to the
// property case.
BEZEL_TEST(increment_reference_type_2585,
           string_local_increment_rejected_spec_17_6_issue_2585) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string := \"hi\"; "
        "    s++; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "`++` is not defined on reference type `string`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§17.6"));
}

// §17.6 / #2585: `k++` on a CLASS-typed local whose class does NOT
// overload `++`. The diagnostic names the user-declared class so the user
// can find the (missing) overload. Pre-fix ICE: same sadd.with.overflow
// on the class pointer.
BEZEL_TEST(increment_reference_type_2585,
           class_local_increment_rejected_spec_17_6_issue_2585) {
    auto r = parseSemaAll(
        "module M; class K end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(); "
        "    k++; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "`++` is not defined on reference type `K`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§17.6"));
}

// §17.6 / #2585: the `--` twin. `s--` on a string local must reject with
// the same §17.6 rule (the operator token is `--`, not `++`).
BEZEL_TEST(increment_reference_type_2585,
           string_local_decrement_rejected_spec_17_6_issue_2585) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string := \"hi\"; "
        "    s--; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "`--` is not defined on reference type `string`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§17.6"));
}

// §17.6 / #2585: an Object-typed computed property — Object is a reference
// type per §4 (and `isNonNullableReferenceType`'s Primitive::Object arm).
// Proves the check is not string-only: every reference-type primitive is
// rejected when it has no overload.
BEZEL_TEST(increment_reference_type_2585,
           object_property_increment_rejected_spec_17_6_issue_2585) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public _o: Object := new Object(); "
        "  public property P: Object "
        "    get => _o; "
        "    set => do _o := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    c.P--; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "`--` is not defined on reference type `Object`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§17.6"));
}

// =========================================================================
// §17.6 — ACCEPT CONTROLS (a false reject here is worse than the ICE)
// =========================================================================

// §17.6 / §6.1: `i++` on an int local — "For built-in numeric types,
// `x++` is equivalent to `x := x + 1`". MUST still compile.
BEZEL_TEST(increment_reference_type_2585,
           int_local_increment_accepted_spec_17_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var i := 5; "
        "    i++; "
        "    return i; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §17.6 / §6.1: `l++` on a long local — the broader integer tower MUST
// still compile.
BEZEL_TEST(increment_reference_type_2585,
           long_local_increment_accepted_spec_17_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var l: long := 1; "
        "    l++; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §17.6 / §6.2: `d++` on a double local — floating-point tower MUST still
// compile.
BEZEL_TEST(increment_reference_type_2585,
           double_local_increment_accepted_spec_17_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: double := 1.0; "
        "    d++; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §17.6 / §4.5: `ch++` on a char local — char is a value-type primitive
// with built-in `++` (§6.1). MUST still compile.
BEZEL_TEST(increment_reference_type_2585,
           char_local_increment_accepted_spec_17_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var ch: char := 'a'; "
        "    ch++; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §17.6 / §15.1: `arr[0]++` on an inline-array int element — value-type
// element, built-in `++`. MUST still compile.
BEZEL_TEST(increment_reference_type_2585,
           inline_array_element_increment_accepted_spec_17_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var arr: [3]int; "
        "    arr[0]++; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §17.6 / §25.3 (the `Counter` example): a RECORD that overloads
// `operator ++` — `c++` desugars to `c := Counter.operator++(c)`. The
// overload-lookup path MUST still admit this (the new check skips when
// `hasOverload` is true). This is the canonical no-false-reject guard
// for the value-type-with-overload axis.
BEZEL_TEST(increment_reference_type_2585,
           record_with_operator_increment_accepted_spec_17_6_spec_25_3) {
    auto r = parseSemaAll(
        "module M; "
        "record Counter(value: int) "
        "  public static method operator ++(c: Counter): Counter "
        "    => new Counter(c.value + 1); "
        "end Counter; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Counter(0); "
        "    c++; "
        "    return c.value; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §17.6 (the reference-type admit clause): a CLASS that overloads
// `operator ++` — "Overloading `++`/`--` on a class is permitted but
// uncommon" — `c++` MUST still compile. This is the PRIMARY
// no-false-reject axis for the new reference-type branch: a class is a
// reference type, so the new check FIRES, finds the user-declared
// overload, and admits the statement. A false reject here would break
// the §17.6 class-overload idiom the spec explicitly admits.
BEZEL_TEST(increment_reference_type_2585,
           class_with_operator_increment_accepted_spec_17_6) {
    auto r = parseSemaAll(
        "module M; "
        "class Counter "
        "  public value: int := 0; "
        "  public constructor(v: int) value := v; end constructor; "
        "  public static method operator ++(c: Counter): Counter "
        "    => new Counter(c.value + 1); "
        "end Counter; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Counter(0); "
        "    c++; "
        "    return c.value; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
