// Spec-derived tests for issue #4419 — §7.9 (v0.299, #4419) "Default
// initialization" of LOCAL declarations: a local of DEFAULTABLE type
// declared without an initializer holds that type's default from its
// declaration and may be read; a local of NON-defaultable type still
// requires explicit assignment before read.
//
// Pre-#4419 DefiniteAssignment.cpp (visitStmt's LocalDecl arm at
// src/sema/DefiniteAssignment.cpp:1206-1222) marked a no-init local as
// definitely-assigned ONLY for `TypeRef::Kind::InlineArray`, so both
// `var n: int;` and `var label: string?;` drew `read of unassigned
// local` — over-strict by both v0.298 §7.9's prose and v0.299's
// "Default initialization" subsection. This fix generalizes the
// InlineArray carve-out to "defaultable" via `sema::typeHasNoDefault`
// (§17.11's authoritative defaultable implementation, fix #4166
// review follow-up), so every defaultable local — int, bool, char,
// numeric tower, nullable types, value-typed records/enums/inline-arrays
// with defaultable components, the unit type — now reads its §17.11
// default; non-defaultable types (non-nullable references, choices,
// records/inline-arrays with non-defaultable components) retain the
// explicit-assignment rule.
//
// Cited: Bezel_Language_Spec.md §7.9 (v0.299, default initialization),
// §17.11 (defaultable).

#include "support/aot_support.hpp"
#include "support/da_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckDA;
using bezel::test::runAot;

namespace {

std::string wrap(const std::string& body) {
    return std::string("module M; "
        "class C public method M(cond: bool) ") + body + " end M; end C;";
}

} // namespace

// T1: `int x;` then `return x;` — int is defaultable (§17.11 row 1),
// default is 0, so the read after the declaration is legal.
BEZEL_TEST(sema_da, int_local_no_init_holds_zero_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap("var x: int; _ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T1b: `bool b;` then `return b;` — bool is defaultable (§17.11 row 2),
// default is false.
BEZEL_TEST(sema_da, bool_local_no_init_holds_false_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap("var b: bool; _ := b;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T1c: `char c;` then `return c;` — char is defaultable (§17.11 row 3),
// default is '\0'.
BEZEL_TEST(sema_da, char_local_no_init_holds_nul_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap("var c: char; _ := c;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T2: nullable reference — `string? s;` is defaultable (§17.11 "every
// nullable type" — the reference's default is null).
BEZEL_TEST(sema_da, nullable_string_local_no_init_holds_null_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap("var s: string?; _ := s;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T2b: nullable value type — `int? n;` is defaultable, default is null.
BEZEL_TEST(sema_da, nullable_int_local_no_init_holds_null_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap("var n: int?; _ := n;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T2c: nullable unit — `()? u;` is defaultable (§17.11 final row).
BEZEL_TEST(sema_da, nullable_unit_local_no_init_holds_null_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap("var u: ()?; _ := u;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T3: CONTROL — non-nullable `string` (a reference type per §4.6,
// NOT in §17.11's "defaultable" list — the bullet "Not defaultable:
// non-nullable reference types" applies). `var s: string;` without an
// initializer still rejects. This is the negation that proves T1/T2
// above are not vacuous — the relaxed predicate is "defaultable", not
// "any type".
BEZEL_TEST(sema_da,
           nonnullable_string_local_no_init_still_diagnoses_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap("var s: string; _ := s;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("read of unassigned local `s`") != std::string::npos)
            return;
    BEZEL_EXPECT_TRUE(false);  // never reached if expected diagnostic fired
}

// T3b: CONTROL — a record local with at least one non-defaultable field
// (non-nullable `string` component) is NOT defaultable (§17.11 "Not
// defaultable: a record any of whose declared fields is not
// defaultable"). `var r: RecNonDefaultable;` without an initializer
// rejects.
BEZEL_TEST(sema_da,
           record_with_nondefaultable_field_no_init_still_diagnoses_spec_7_9_4419) {
    auto r = parseAndCheckDA(
        "module M; "
        "public record RecNonDefaultable(public S: string) end RecNonDefaultable; "
        "class C "
        "  public method M() "
        "    var r: RecNonDefaultable; _ := r.S; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// T3c: CONTROL — an inline array of non-defaultable element
// (non-nullable `string` element) is NOT defaultable (§17.11 "Not
// defaultable: an inline array whose element type is not
// defaultable"). `var a: [3]string;` without an initializer rejects.
// This is a NARROWING relative to the previous InlineArray-only
// carve-out, which silently admitted every `[N]T` regardless of T.
BEZEL_TEST(sema_da,
           inline_array_nondefaultable_element_no_init_still_diagnoses_spec_7_9_4419) {
    auto r = parseAndCheckDA(
        "module M; "
        "class C "
        "  public method M() "
        "    var a: [3]string; _ := a[0]; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// T4: a default-init local may be ASSIGNED later and then read — the
// `x := 5;` re-assignment is a normal write, the prior default of 0
// is overwritten, the read observes the new value. (Idempotent with
// the pre-fix behavior on the assign-then-read shape.)
BEZEL_TEST(sema_da,
           defaultable_local_assign_then_read_still_accepted_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap("var x: int; x := 5; _ := x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T5: a defaultable local declared inside a branch and read on a
// path where the branch may not have executed — the default-initialised
// value (not the branch-assigned value) is what the read observes, so
// no DA error fires. This is the issue's headline scenario:
// `var n: int; if cond then n := 1; end; _ := n;` is now accepted
// because n holds 0 on the `!cond` path.
BEZEL_TEST(sema_da,
           defaultable_local_branch_partial_assign_accepted_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap(
        "var n: int; "
        "if cond then n := 1; end; "
        "_ := n;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// T6: a defaultable local in an INLINE-array slot — `[3]int` (all-
// defaultable element) holds the per-slot zero default, so a read
// after the declaration is legal. Replaces the previous
// InlineArray-only carve-out (which silently admitted even non-
// defaultable element types).
BEZEL_TEST(sema_da,
           inline_array_defaultable_element_no_init_accepted_spec_7_9_4419) {
    auto r = parseAndCheckDA(wrap(
        "var a: [3]int; _ := a[0];"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =====================================================================
// RUN-AOT PIN — the DA-acceptance rows above prove the §7.9 admit, but
// DA marking alone does not prove codegen actually zero-initialises the
// local slot. These rows compile, RUN, and assert the §17.11 default
// observable in stdout. A defect that turns the read of unassigned local
// from a compile error into silent garbage or a UAF would show up here.
//
// Every row asserts exact stdout AND `r.ran == true` AND `r.exitCode == 0`.
// If codegen leaves a slot uninitialised, the printed value diverges from
// the §17.11 default and the row fails — pinning the bug the DA-only
// tests could not.
// =====================================================================

// RT1 — `int n;` then `Console.WriteLine(n);` must print "0".
BEZEL_TEST(sema_da,
           rundaot_int_local_no_init_reads_zero_spec_7_9_4419) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var n: int; "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// RT2 — `double d;` then `Console.WriteLine(d);` must print "0".
BEZEL_TEST(sema_da,
           rundaot_double_local_no_init_reads_zero_spec_7_9_4419) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var d: double; "
        "  Console.WriteLine(d); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// RT3 — `bool b;` then `Console.WriteLine(b);` must print "False".
BEZEL_TEST(sema_da,
           rundaot_bool_local_no_init_reads_false_spec_7_9_4419) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var b: bool; "
        "  Console.WriteLine(b); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

// RT4 — `string? s;` then null check must print "null".
BEZEL_TEST(sema_da,
           rundaot_nullable_string_local_no_init_is_null_spec_7_9_4419) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var s: string?; "
        "  if s = null then Console.WriteLine(\"null\"); "
        "  else Console.WriteLine(\"notnull\"); "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("null\n"));
}

// RT5 — `SomeClass? o;` (a user class) — same null check must print
// "null". A user class with `?` is a nullable reference; its §17.11
// default is null.
BEZEL_TEST(sema_da,
           rundaot_nullable_class_local_no_init_is_null_spec_7_9_4419) {
    auto r = runAot(
        "module M; "
        "class SomeClass "
        "  public X: int; "
        "  public constructor() X := 0; end constructor; "
        "end SomeClass; "
        "class C public static method Main(): int "
        "  var o: SomeClass?; "
        "  if o = null then Console.WriteLine(\"null\"); "
        "  else Console.WriteLine(\"notnull\"); "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("null\n"));
}

// RT6 — a defaultable record/struct value local — its fields read as
// their own §17.11 defaults: `I: int` → 0, `S: string?` → null.
BEZEL_TEST(sema_da,
           rundaot_defaultable_record_local_fields_zero_spec_7_9_4419) {
    auto r = runAot(
        "module M; "
        "public record R(I: int, S: string?); "
        "class C public static method Main(): int "
        "  var r: R; "
        "  Console.WriteLine(r.I); "
        "  if r.S = null then Console.WriteLine(\"null\"); "
        "  else Console.WriteLine(\"notnull\"); "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\nnull\n"));
}

// RT7 — `[3]int a;` then `$"{a[0]},{a[1]},{a[2]}"` must print "0,0,0".
// Inline-array of all-defaultable elements (§17.11 narrows the previous
// over-permissive carve-out to defaultability of the element type).
BEZEL_TEST(sema_da,
           rundaot_inline_array_int_local_zero_each_spec_7_9_4419) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  var a: [3]int; "
        "  Console.WriteLine($\"{a[0]},{a[1]},{a[2]}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0,0,0\n"));
}

// RT8 — the issue's headline shape: a defaultable local assigned on ONE
// branch but read on a path that may not have executed. The read must
// observe the §17.11 default (0), NOT garbage or a runtime error.
BEZEL_TEST(sema_da,
           rundaot_one_branch_assign_unassigned_reads_zero_spec_7_9_4419) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method someCondition(): bool return false; end someCondition; "
        "  public static method Main(): int "
        "    var n: int; "
        "    if someCondition() then n := 42; end if; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// RT9 — 200-iteration loop-carried shape — the STALE-SLOT oracle. A
// local declared inside a per-iteration body must be re-initialised to
// its §17.11 default on each iteration; if the slot is reused from a
// previous iteration's leftover, an iteration that does NOT assign would
// observe a stale non-zero value and this row would print "STALE at iter
// I" and exit 1.
BEZEL_TEST(sema_da,
           rundaot_loop_carried_local_reinitialized_each_iter_spec_7_9_4419) {
    auto r = runAot(
        "module M; "
        "class C public static method Main(): int "
        "  for var i in 0..200 do "
        "    var x: int; "
        "    if i mod 2 = 0 then x := i; end if; "
        "    if i mod 2 <> 0 then "
        "      if x <> 0 then "
        "        Console.WriteLine($\"STALE at iter {i}: x={x}\"); "
        "        return 1; "
        "      end if; "
        "    end if; "
        "  end for; "
        "  Console.WriteLine(\"loop-carried OK\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("loop-carried OK\n"));
}
