// Codegen — fix #428 phase a: `this with … end` inside a GENERIC record
// instance method (§9.5 / §14.2).
//
// Root cause: when `buildSpecializedMethodFn` compiles a generic record method
// for a concrete instantiation (e.g. Box<int>), it creates a fresh specFn NOT
// registered in methodFns_.  The `#575` reverse-lookup in lowerWith looks for
// `fn == currentFn_` in methodFns_ to recover the parent record TypeSymbol; it
// finds nothing → t = nullptr → lowerWith returns the raw `this` pointer →
// `ret ptr %0` where the specialized function expects `ret %record.Box$spec0` →
// LLVM IR verification failure.
//
// Fix: buildSpecializedMethodFn stores the original MethodSymbol* in
// currentSpecMethodSymbol_ for the duration of the body; lowerWith's fallback
// path checks currentSpecMethodSymbol_->parent when the methodFns_ reverse-
// lookup fails.
//
// All tests use only real stdlib surface. Each test would IR-fail (LLVM
// assertion / verifier error) before the fix and passes after; the always-on
// IR verifier embedded in the AOT driver proves the IR is well-formed.
//
// Spec references:
//   §9.5  v0.135 — record `with` expression (non-destructive copy)
//   §9.4  v0.135 — record instance methods receive `this` by pointer (CG29)
//   §14.2 v0.135 — generics are monomorphized per concrete type argument

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// ══════════════════════════════════════════════════════════════════════════════
// Box<int> — baseline (was already passing before the fix because T→i32
// placeholder coincides with int; keep as regression guard that our fix does
// not break the int path).
// ══════════════════════════════════════════════════════════════════════════════

// §9.5 / §14.2: Box<int> with-method returns updated value; int field survives.
BEZEL_TEST(generic_record_this_with_method, box_int_with_value_returns_updated) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Box<T>(value: T) "
        "  public method WithValue(v: T): Box<T> => this with value := v end; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(1); "
        "  var b2 := b.WithValue(99); "
        "  return b2.value; "
        "end Main; end C;"), 99);
}

// §9.5: source Box<int> is NOT mutated by the with-method.
BEZEL_TEST(generic_record_this_with_method, box_int_with_does_not_mutate_source) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Box<T>(value: T) "
        "  public method WithValue(v: T): Box<T> => this with value := v end; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(1); "
        "  var b2 := b.WithValue(99); "
        "  return b.value * 100 + b2.value; "  // 1*100 + 99 = 199
        "end Main; end C;"), 199);
}

// ══════════════════════════════════════════════════════════════════════════════
// Box<string> — this is the previously CRASHING shape: T → ptr (8 bytes),
// placeholder was i32 (4 bytes), field write truncated the pointer → SIGSEGV.
// ══════════════════════════════════════════════════════════════════════════════

// §9.5 / §14.2: Box<string> with-method; replaced string pointer survives,
// .Length returns correct value. "world".Length = 5.
BEZEL_TEST(generic_record_this_with_method, box_string_with_value_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Box<T>(value: T) "
        "  public method WithValue(v: T): Box<T> => this with value := v end; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<string>(\"hi\"); "
        "  var b2 := b.WithValue(\"world\"); "
        "  return b2.value.Length; "
        "end Main; end C;"), 5);
}

// §9.5: source Box<string> is unchanged after the with-method (source "hi"
// Length 2, replacement "world" Length 5 → 2*100 + 5 = 205).
BEZEL_TEST(generic_record_this_with_method, box_string_with_source_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Box<T>(value: T) "
        "  public method WithValue(v: T): Box<T> => this with value := v end; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<string>(\"hi\"); "
        "  var b2 := b.WithValue(\"world\"); "
        "  return b.value.Length * 100 + b2.value.Length; "  // 2*100 + 5 = 205
        "end Main; end C;"), 205);
}

// §9.5: string content round-trips (not just Length). "planet".Length = 6.
BEZEL_TEST(generic_record_this_with_method, box_string_with_roundtrips_content) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Box<T>(value: T) "
        "  public method WithValue(v: T): Box<T> => this with value := v end; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<string>(\"hi\"); "
        "  var b2 := b.WithValue(\"planet\"); "
        "  return (1 if b2.value = \"planet\" else 0); "
        "end Main; end C;"), 1);
}

// ══════════════════════════════════════════════════════════════════════════════
// Pair<A,B> two-parameter generic record — with-method replacing one field at
// a time, other field must survive.
// ══════════════════════════════════════════════════════════════════════════════

// §9.5 / §14.2: Pair<int,string> with-method replacing the int field (A).
// a=7, b="hello".Length=5 → 7*100 + 5 = 705.
BEZEL_TEST(generic_record_this_with_method, pair_int_string_replace_first) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "record Pair<A, B>(first: A, second: B) "
        "  public method WithFirst(v: A): Pair<A, B> "
        "    => this with first := v end; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p := new Pair<int, string>(1, \"hello\"); "
        "  var p2 := p.WithFirst(7); "
        "  Console.WriteLine(p2.first * 100 + p2.second.Length); return 0; "  // 7*100 + 5 = 705
        "end Main; end C;"), "705");
}

// §9.5 / §14.2: Pair<int,string> with-method replacing the string field (B).
// The int field must survive unchanged. first=3, "planet".Length=6 → 3*100 + 6 = 306.
BEZEL_TEST(generic_record_this_with_method, pair_int_string_replace_second) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "record Pair<A, B>(first: A, second: B) "
        "  public method WithSecond(v: B): Pair<A, B> "
        "    => this with second := v end; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p := new Pair<int, string>(3, \"hello\"); "
        "  var p2 := p.WithSecond(\"planet\"); "
        "  Console.WriteLine(p2.first * 100 + p2.second.Length); return 0; "  // 3*100 + 6 = 306
        "end Main; end C;"), "306");
}

// §9.5 / §14.2: Pair<string,int> — ptr+int ordering (ptr in slot 0, int in
// slot 1). With-method on int field (second); string field must survive.
// "hi".Length=2, second=99 → 2*100 + 99 = 299.
BEZEL_TEST(generic_record_this_with_method, pair_string_int_replace_int) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "record Pair<A, B>(first: A, second: B) "
        "  public method WithSecond(v: B): Pair<A, B> "
        "    => this with second := v end; "
        "end Pair; "
        "class C public static method Main(): int "
        "  var p := new Pair<string, int>(\"hi\", 1); "
        "  var p2 := p.WithSecond(99); "
        "  Console.WriteLine(p2.first.Length * 100 + p2.second); return 0; "  // 2*100 + 99 = 299
        "end Main; end C;"), "299");
}

// ══════════════════════════════════════════════════════════════════════════════
// Chained with-method calls — ensures the return value of the first call
// correctly becomes the receiver for the second call.
// ══════════════════════════════════════════════════════════════════════════════

// §9.5 / §14.2: chain two Box<int> with-method calls; value is updated twice.
// b.WithValue(10).WithValue(99) → 99.
BEZEL_TEST(generic_record_this_with_method, box_int_chained_with_calls) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Box<T>(value: T) "
        "  public method WithValue(v: T): Box<T> => this with value := v end; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(1); "
        "  return b.WithValue(10).WithValue(99).value; "
        "end Main; end C;"), 99);
}

// §9.5 / §14.2: chain two Box<string> with-method calls; string pointer
// survives both. Final .Length == "planet".Length == 6.
BEZEL_TEST(generic_record_this_with_method, box_string_chained_with_calls) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Box<T>(value: T) "
        "  public method WithValue(v: T): Box<T> => this with value := v end; "
        "end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<string>(\"hi\"); "
        "  return b.WithValue(\"world\").WithValue(\"planet\").value.Length; "
        "end Main; end C;"), 6);
}

// ══════════════════════════════════════════════════════════════════════════════
// Coexistence: Box<int> and Box<string> specialized bodies in the same module.
// Regression guard: the two specializations must not collide in the cache.
// ══════════════════════════════════════════════════════════════════════════════

// §9.5 / §14.2: both Box<int> and Box<string> with-methods in one module.
// bi.WithValue(42).value = 42; bs.WithValue("hello").value.Length = 5 → 42*100+5=4205.
BEZEL_TEST(generic_record_this_with_method, box_int_and_box_string_coexist) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "record Box<T>(value: T) "
        "  public method WithValue(v: T): Box<T> => this with value := v end; "
        "end Box; "
        "class C public static method Main(): int "
        "  var bi := new Box<int>(1); "
        "  var bs := new Box<string>(\"x\"); "
        "  Console.WriteLine(bi.WithValue(42).value * 100 + bs.WithValue(\"hello\").value.Length); return 0; "
        "end Main; end C;"), "4205");
}
