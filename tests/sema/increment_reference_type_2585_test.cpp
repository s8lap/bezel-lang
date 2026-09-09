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
// types, `x++` is equivalent to `x := x + 1`").
//
// ⚠ RETIRED by fix #3978 (§25.1/§17.6, v0.295): rows 11/12 below pinned "a
// record/class that DOES overload `++` still resolves through the
// overload (the §17.6 `Counter` example, §25.3)" as an ACCEPT control —
// that was the v0.236 reading of §17.6, which v0.295 retires outright:
// `++`/`--` are NEVER user-overloadable (§25.1), so `operator ++` is now
// rejected at its own declaration too. Rows 11/12 are FLIPPED to REJECT
// (see their own comments) rather than removed, so the file keeps a
// regression pin on the retired escape hatch.
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
//   REJECT shapes added by fix #3978 (§25.1/§17.6, v0.295 — the retired
//   v0.236 escape hatch, formerly rows 11/12's ACCEPT controls):
//   11. `c++` on a record that overloads `operator ++` — now REJECTED at
//       both the declaration (§25.1) and the use (§17.6)
//   12. `c++` on a CLASS that overloads `operator ++` — likewise REJECTED

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

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "a RECORD that overloads `operator ++` ... the overload-lookup
// path MUST still admit this." That reading is the v0.236 rule §25.1/§17.6
// RETIRE at v0.295: `++`/`--` are never user-overloadable, so `operator ++`
// is rejected at its OWN declaration (checkOperatorGroups, §25.1) and `c++`
// is rejected independently at the use site too (§17.6) — this row now pins
// BOTH rejections on the same program that used to compile clean.
BEZEL_TEST(increment_reference_type_2585,
           record_with_operator_increment_rejected_spec_25_1_issue_3978) {
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
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "operator `++` may not be overloaded (§25.1)"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "`++` is not defined on record type `Counter`"));
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "a CLASS that overloads `operator ++` ... `c++` MUST still
// compile ... A false reject here would break the §17.6 class-overload
// idiom the spec explicitly admits." §25.1/§17.6 retire that idiom entirely
// at v0.295 — this is now the PRIMARY reject axis for the reference-type
// branch, not an accept control.
BEZEL_TEST(increment_reference_type_2585,
           class_with_operator_increment_rejected_spec_25_1_issue_3978) {
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
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "operator `++` may not be overloaded (§25.1)"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "`++` is not defined on reference type `Counter`"));
}

// =========================================================================
// §17.6 / #2599 — value-type RECORD sibling (pre-fix: codegen ICE on
// `%record.R`, same malformed-`sadd.with.overflow` shape as #2585)
//
// §17.6 (v0.236) enumerates exactly three operand categories on which
// `++`/`--` are defined:
//   (1) "a value-type operand whose type T overloads `operator ++`"
//   (2) "built-in numeric types" (`x++` ≡ `x := x + 1`)
//   (3) "reference-type operands [that] overload the operator"
// A user record is a value type (§9) that is NOT a built-in numeric, so
// §17.6's category (1) is the ONLY clause that could admit `r++` — and it
// applies IFF the record overloads `operator ++`. A record WITHOUT the
// overload therefore matches none of §17.6's three categories and is
// invalid by exhaustion (the basis is §17.6 itself, by case exhaustion —
// not the reference-type clause literally, which is why the diagnostic
// wording says "value-type record" rather than "reference type").
//
// Pre-fix (#2599): sema let `r++` through (the #2585 check was DELIBERATELY
// scoped to reference types — records have a different TypeDeclKind and
// fell through to `default`), and codegen's IncrementStmt handler reached
// the "Built-in numeric" branch with `elemTy = %record.R = type { i32 }`,
// building `ConstantInt::get(%record.R, 1)` — a degenerate `i33 1` — and
// feeding it to `emitCompoundOp` -> `emitCheckedArith(sadd.with.overflow,
// %record.R, i33)`, producing malformed IR
// (`@llvm.sadd.with.overflow.s_record.Rs(%record.R %4, i33 1)`) that
// verifyModule rejects (ICE) — the value-type sibling of #2585's
// pointer-type ICE.
//
// The fix extends the #2585 IncrementStmt reject in ExprTypecheck.cpp to
// also fire for a value-type RECORD (TypeDeclKind::Record) operand that
// has no `operator ++`/`operator --` overload, with a §17.6 diagnostic
// whose wording names the value-type-record category. Numeric primitives
// keep their built-in `++` (the check's `default` branch skips them).
//
// ⚠ RETIRED by fix #3978 (§25.1/§17.6, v0.295): row 5 below pinned "a
// record that DOES overload `++` (§25.3 `Counter`) is admitted by the
// `hasOverload` guard — the key false-reject axis" as an ACCEPT control.
// v0.295 retires that guard outright — `++`/`--` are NEVER
// user-overloadable — so row 5 is FLIPPED to REJECT below.
//
// Tests (REJECT = the bug; ACCEPT = no-false-reject control):
//   REJECT shapes (§17.6 violation — was an ICE pre-fix):
//    1. `r++` on a record local whose record does NOT overload ++ (the
//       issue's repro)
//    2. `r--` on a record local (the `--` twin)
//    3. `f++` on a record-typed FIELD (proves the check fires on a
//       non-local target too — same code path, different lvalue category)
//   ACCEPT shapes (no false reject — a regression here is worse than #2599):
//    4. `i++` on an int local (built-in numeric — §17.6 category (2))
//   REJECT shape added by fix #3978 (formerly row 5's ACCEPT control):
//    5. `c++` on a record that overloads `operator ++` (§25.3 `Counter` —
//       overload MUST still compile)
// =========================================================================

// §17.6 / #2599 primary repro: `r++` on a record local whose record does
// NOT overload `operator ++`. Pre-fix: codegen built
// `ConstantInt::get(%record.R, 1)` -> `i33 1` ->
// `@llvm.sadd.with.overflow.s_record.Rs(%record.R, i33 1)` -> verifyModule
// rejected the IR. Post-fix: clean sema REJECT citing §17.6.
BEZEL_TEST(increment_record_value_type_2599,
           record_local_increment_rejected_spec_17_6_issue_2599) {
    auto r = parseSemaAll(
        "module M; record R(x: int) end R; "
        "class C "
        "  public static method Main(): int "
        "    var r := new R(1); "
        "    r++; "
        "    return r.x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "`++` is not defined on record type `R`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§17.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "value-type record"));
}

// §17.6 / #2599: the `--` twin. `r--` on a record local must reject with
// the same §17.6 rule (the operator token is `--`, not `++`).
BEZEL_TEST(increment_record_value_type_2599,
           record_local_decrement_rejected_spec_17_6_issue_2599) {
    auto r = parseSemaAll(
        "module M; record R(x: int) end R; "
        "class C "
        "  public static method Main(): int "
        "    var r := new R(1); "
        "    r--; "
        "    return r.x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "`--` is not defined on record type `R`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§17.6"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "value-type record"));
}

// §17.6 / #2599: `f++` on a record-typed FIELD — same defect on a
// non-local target (the check keys on the target's resolved type, not the
// lvalue category, so the field path is covered too). A field of record
// type without an overload must reject identically.
BEZEL_TEST(increment_record_value_type_2599,
           record_field_increment_rejected_spec_17_6_issue_2599) {
    auto r = parseSemaAll(
        "module M; record R(x: int) end R; "
        "class C "
        "  public f: R := new R(0); "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    c.f++; "
        "    return c.f.x; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "`++` is not defined on record type `R`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§17.6"));
}

// §17.6 / §6.1: `i++` on an int local — "For built-in numeric types,
// `x++` is equivalent to `x := x + 1`". MUST still compile — the numeric
// value-type primitive is §17.6 category (2), skipped by the new check's
// `default` branch. (Restated here in the #2599 suite so the record
// reject's no-false-reject story is self-contained, independent of the
// #2585 suite above.)
BEZEL_TEST(increment_record_value_type_2599,
           int_local_increment_accepted_spec_17_6_issue_2599) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var i := 5; "
        "    i++; "
        "    return i; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "the KEY no-false-reject axis for the #2599 record branch ...
// A false reject here would break the §25.3 `Counter` idiom." §25.1/§17.6
// retire the idiom at v0.295 (this is the THIRD occurrence of the same
// now-illegal shape in this file — #2585's suite above pins the same
// program's rejection with the record and class variants).
BEZEL_TEST(increment_record_value_type_2599,
           record_with_operator_increment_rejected_spec_25_1_issue_3978) {
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
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "operator `++` may not be overloaded (§25.1)"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "`++` is not defined on record type `Counter`"));
}
