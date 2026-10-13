// #1586 (stdlib Sec 1 / Sec 4.4, v0.183, #1396 follow-up): a negative
// capacity/size/length magnitude passed to a constructor raises
// ArgumentOutOfRangeException instead of silently falling back to an empty
// container. Every constructor stdlib Sec 1's "Negative magnitudes fault"
// bullet names is covered: StringBuilder(capacity), Array<T>(capacity),
// Array<T>(size, fill), OwningArray<T>(capacity), Dictionary<K,V>(capacity),
// OwningDictionary<K,V>(capacity), Set<T>(capacity), OwningBuffer<T>(length)
// and its (length, init) overload. Zero remains valid (an empty container).
//
// ArgumentOutOfRangeException itself is new to the compiler (stdlib Sec 2.5
// declares it, deriving from ArgumentException) -- this slice registers it as
// a builtin exception type alongside ArgumentException/ArgumentNullException
// so `throw`/`catch`/vtable dispatch all work, then wires the negative-
// magnitude guard at each constructor site.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// A minimal class T for the T:class-constrained owning-collection forms
// (OwningArray<T>, OwningDictionary<K,V> where V: class).
int runWithFoo(const std::string& body) {
    auto r = runAot(
        std::string("module M; class Foo public v: int := 0; end Foo; "
                     "class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ---------------------------------------------------------------------------
// Array<T>(capacity) / Array<T>(size, fill)
// ---------------------------------------------------------------------------

BEZEL_TEST(negative_capacity_aoor_1396, array_negative_capacity_raises) {
    BEZEL_EXPECT_EQ(run(
        "try var a := new Array<int>(-1); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

BEZEL_TEST(negative_capacity_aoor_1396, array_negative_size_fill_raises) {
    BEZEL_EXPECT_EQ(run(
        "try var a := new Array<int>(-1, 7); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

// Control: capacity 0 is a valid empty array, no exception.
BEZEL_TEST(negative_capacity_aoor_1396, array_zero_capacity_is_valid) {
    BEZEL_EXPECT_EQ(run(
        "using var a := new Array<int>(0); return a.Count;"), 0);
}

// ---------------------------------------------------------------------------
// OwningArray<T>(capacity) -- T: class
// ---------------------------------------------------------------------------

BEZEL_TEST(negative_capacity_aoor_1396, owning_array_negative_capacity_raises) {
    BEZEL_EXPECT_EQ(runWithFoo(
        "try var a := new OwningArray<Foo>(-1); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

// ---------------------------------------------------------------------------
// StringBuilder(capacity)
// ---------------------------------------------------------------------------

BEZEL_TEST(negative_capacity_aoor_1396, string_builder_negative_capacity_raises) {
    BEZEL_EXPECT_EQ(run(
        "try var sb := new StringBuilder(-1); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

// Control: capacity 0 is valid.
BEZEL_TEST(negative_capacity_aoor_1396, string_builder_zero_capacity_is_valid) {
    BEZEL_EXPECT_EQ(run(
        "using var sb := new StringBuilder(0); sb.Append(\"x\"); "
        "return sb.Length;"), 1);
}

// StringBuilder(initial: string) must still work -- the negative-capacity
// guard must not misfire on the string-argument overload.
BEZEL_TEST(negative_capacity_aoor_1396, string_builder_string_ctor_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "using var sb := new StringBuilder(\"hi\"); return sb.Length;"), 2);
}

// ---------------------------------------------------------------------------
// OwningBuffer<T>(length) and (length, init)
// ---------------------------------------------------------------------------

BEZEL_TEST(negative_capacity_aoor_1396, owning_buffer_negative_length_raises) {
    BEZEL_EXPECT_EQ(run(
        "try var b := new OwningBuffer<int>(-1); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

BEZEL_TEST(negative_capacity_aoor_1396, owning_buffer_negative_length_init_raises) {
    BEZEL_EXPECT_EQ(run(
        "try var b := new OwningBuffer<int>(-1, lambda(i) => i); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

// Control: length 0 is a valid empty buffer. Read through `Length` (stdlib
// §4.14): `OwningBuffer<T>` declares no `Count`, so the bare `.Count` read this
// row used to make is now a "no member" reject (fix #6081; see
// sema/member_set_residual_6081_test.cpp R5). The expected value is unchanged.
BEZEL_TEST(negative_capacity_aoor_1396, owning_buffer_zero_length_is_valid) {
    BEZEL_EXPECT_EQ(run(
        "using var b := new OwningBuffer<int>(0); return b.Length;"), 0);
}

// ---------------------------------------------------------------------------
// Dictionary<K,V>(capacity) / OwningDictionary<K,V>(capacity)
// ---------------------------------------------------------------------------

BEZEL_TEST(negative_capacity_aoor_1396, dictionary_negative_capacity_raises) {
    BEZEL_EXPECT_EQ(run(
        "try var d := new Dictionary<string, int>(-1); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

BEZEL_TEST(negative_capacity_aoor_1396, owning_dictionary_negative_capacity_raises) {
    BEZEL_EXPECT_EQ(runWithFoo(
        "try var d := new OwningDictionary<string, Foo>(-1); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

// Control: capacity 0 is valid, still usable.
BEZEL_TEST(negative_capacity_aoor_1396, dictionary_zero_capacity_is_valid) {
    BEZEL_EXPECT_EQ(run(
        "using var d := new Dictionary<string, int>(0); "
        "d.Add(\"a\", 1); return d.Count;"), 1);
}

// ---------------------------------------------------------------------------
// Set<T>(capacity)
// ---------------------------------------------------------------------------

BEZEL_TEST(negative_capacity_aoor_1396, set_negative_capacity_raises) {
    BEZEL_EXPECT_EQ(run(
        "try var s := new Set<int>(-1); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

// Control: capacity 0 is valid, still usable.
BEZEL_TEST(negative_capacity_aoor_1396, set_zero_capacity_is_valid) {
    BEZEL_EXPECT_EQ(run(
        "using var s := new Set<int>(0); s.Add(1); return s.Count;"), 1);
}

// ---------------------------------------------------------------------------
// ArgumentOutOfRangeException derives from ArgumentException (stdlib Sec 2.5)
// ---------------------------------------------------------------------------

BEZEL_TEST(negative_capacity_aoor_1396, catchable_as_base_argument_exception) {
    BEZEL_EXPECT_EQ(run(
        "try var a := new Array<int>(-1); return 0; "
        "catch ArgumentException => return 43; end;"), 43);
}
