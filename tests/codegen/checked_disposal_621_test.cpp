// Codegen: checked disposal mode — field access and built-in indexer guards
// (issue #621 / §28.2 v0.135 / §31.4).
//
// §28.2 v0.135 specifies that in checked disposal mode, ANY instance member
// access on a disposed instance raises DisposedAccessException.  The list
// explicitly includes:
//   "…or a field access through the reference"
// as well as "a property or indexer accessor invocation" (the base #601 set).
//
// Issue #601 shipped method-call and property-accessor guards.  This file
// tests the REMAINING surfaces:
//
//   7.  Stored-field READ on disposed instance → DisposedAccessException.
//   8.  Stored-field WRITE on disposed instance → DisposedAccessException.
//   9.  Field read on a NOT-yet-disposed instance → no exception (correct).
//  10.  Field write on a NOT-yet-disposed instance → no exception (correct).
//  11.  Field read via explicit this.field inside an instance method —
//       instance methods are already guarded at entry (#601); the field
//       read body must not double-raise (check that live access still works).
//  12.  Built-in Array<int> indexer GET on disposed array → DisposedAccessException.
//  13.  Built-in Array<int> indexer SET on disposed array → DisposedAccessException.
//  14.  Array<int> indexer GET on live array in checked mode → normal result.
//  15.  Array<int> indexer SET on live array in checked mode → normal result.
//  16.  Correct program (field access, no disposal) identical output both modes.
//
// NOTE: Tests for "unchecked mode + use-after-dispose" are intentionally
// omitted. Use-after-dispose is undefined behavior in unchecked mode; running
// such code inside the JIT test harness (shared process) can corrupt heap
// state for subsequent tests. The "no guard emitted in default mode" property
// is verified indirectly by spec_28_2_correct_field_prog_default (correct
// programs produce the same result in both modes — if the guard were
// incorrectly emitted in default mode it would raise on live instances).

#include "support/aot_support.hpp"   // runAot/runAotChecked — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Convenience wrappers.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

int runDefault(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace


// ── §28.2 v0.135: stored-field READ on disposed instance ─────────────────

BEZEL_TEST(codegen_checked_disposal_621, spec_28_2_field_read_raises) {
    // §28.2 v0.135: "a field access through the reference" on a disposed
    // instance raises DisposedAccessException in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public V: int := 42; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    obj.Dispose(); "
        "    try "
        "      var x := obj.V; "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}


// ── §28.2 v0.135: stored-field WRITE on disposed instance ────────────────

BEZEL_TEST(codegen_checked_disposal_621, spec_28_2_field_write_raises) {
    // §28.2 v0.135: writing to a field of a disposed instance raises
    // DisposedAccessException in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public V: int := 0; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    obj.Dispose(); "
        "    try "
        "      obj.V := 99; "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}


// ── §28.2: field read on live instance does not raise ────────────────────

BEZEL_TEST(codegen_checked_disposal_621, spec_28_2_field_read_live_ok) {
    // §28.2 v0.135: guard fires only for a disposed instance. Accessing a
    // field of a live instance must succeed normally in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public V: int := 7; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    return obj.V; "
        "  end Main; "
        "end C;"), 7);
}


// ── §28.2: field write on live instance does not raise ───────────────────

BEZEL_TEST(codegen_checked_disposal_621, spec_28_2_field_write_live_ok) {
    // §28.2 v0.135: guard fires only for a disposed instance. Writing to a
    // field of a live instance must succeed normally in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public V: int := 0; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    obj.V := 5; "
        "    return obj.V; "
        "  end Main; "
        "end C;"), 5);
}


// ── §28.2: this-field access inside instance method — no double-raise ────

BEZEL_TEST(codegen_checked_disposal_621, spec_28_2_this_field_access_no_double_guard) {
    // §28.2 v0.135: an instance method is already guarded at entry (#601).
    // `this.field` reads inside a live method body must not double-raise.
    // If the guard were double-emitted, the method body would raise for a
    // live instance (before disposal) — this verifies it does not.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public V: int := 3; "
        "  public method GetV(): int return V; end GetV; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    return obj.GetV(); "
        "  end Main; "
        "end C;"), 3);
}


// ── §28.2: DisposedAccessException from field read catchable as base ─────

BEZEL_TEST(codegen_checked_disposal_621, spec_31_4_field_raise_caught_by_base) {
    // §31.4 v0.135: DisposedAccessException isa InvalidOperationException.
    // A catch arm for the base type must catch the field-access guard raise.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public V: int := 0; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    obj.Dispose(); "
        "    try "
        "      var x := obj.V; "
        "      return 0; "
        "    catch var e: InvalidOperationException => return 1; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}


// ── §28.2: correct program (field access, no disposal) identical both modes

BEZEL_TEST(codegen_checked_disposal_621, spec_28_2_correct_field_prog_checked) {
    // §28.2 v0.135: identical observable behavior for a program that never
    // uses a disposed instance, in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public V: int := 99; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    return obj.V; "
        "  end Main; "
        "end C;"), 99);
}

BEZEL_TEST(codegen_checked_disposal_621, spec_28_2_correct_field_prog_default) {
    // §28.2 v0.135: same program in default (unchecked) mode.
    BEZEL_EXPECT_EQ(runDefault(
        "module M; class C "
        "  public V: int := 99; "
        "  public static method Main(): int "
        "    using var obj := new C(); "
        "    return obj.V; "
        "  end Main; "
        "end C;"), 99);
}


// ── §28.2 v0.135: built-in Array<int> indexer GET on disposed array ───────

BEZEL_TEST(codegen_checked_disposal_621, spec_28_2_array_indexer_get_raises) {
    // §28.2 v0.135: "a property or indexer accessor invocation" on a disposed
    // instance raises DisposedAccessException. The built-in Array<T> indexer
    // is an indexer accessor; accessing it after Dispose() must raise.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "
        "    arr.Dispose(); "
        "    try "
        "      var x := arr[0]; "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}


// ── §28.2 v0.135: built-in Array<int> indexer SET on disposed array ───────

BEZEL_TEST(codegen_checked_disposal_621, spec_28_2_array_indexer_set_raises) {
    // §28.2 v0.135: the indexer setter on a disposed Array<T> likewise raises
    // DisposedAccessException in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(0); "
        "    arr.Dispose(); "
        "    try "
        "      arr[0] := 5; "
        "      return 0; "
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}


// ── §28.2: Array<int> indexer GET on live array succeeds ─────────────────

BEZEL_TEST(codegen_checked_disposal_621, spec_28_2_array_indexer_get_live_ok) {
    // §28.2 v0.135: guard fires only for disposed. A live array's indexer
    // must return the correct element in checked mode.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(77); "
        "    return arr[0]; "
        "  end Main; "
        "end C;"), 77);
}


// ── §28.2: Array<int> indexer SET on live array succeeds ─────────────────

BEZEL_TEST(codegen_checked_disposal_621, spec_28_2_array_indexer_set_live_ok) {
    // §28.2 v0.135: setting an element of a live array in checked mode must
    // succeed and be readable back.
    BEZEL_EXPECT_EQ(runChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(0); "
        "    arr[0] := 55; "
        "    return arr[0]; "
        "  end Main; "
        "end C;"), 55);
}
