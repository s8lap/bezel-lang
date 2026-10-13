// Codegen — issue #5806 (spec §9.2 *Through a type parameter*, v0.338; §15.1.2,
// §8.3, §8.6): inside a generic body, `a.Equals(b)`, `a.Equals(o)` and
// `a.GetHashCode()` on a receiver typed by a type parameter `T` read "the three
// kinds ... at the instantiation". "At an inline-array S, the first kind and
// `GetHashCode` reach §15.1.2's element-wise members, and the second kind reads
// the boxed-argument rule: a boxed S of the same type compares element-wise,
// and any other argument compares unequal".
//
// Before the fix the call reached the declaration-less-builtin backstop of
// tryLowerCall (receiver still the unsubstituted `T`) and the instantiation was
// rejected with "member `Equals` on `T` is declared via the universal Object
// contract ... not yet implemented". Every row asserts the PRINTED booleans,
// both outcomes of each cell, so a constant-true/constant-false lowering fails.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kGenerics =
    "module M; "
    "class C "
    "  public static method Same<T>(a: T, b: T): bool => a.Equals(b); "
    "  public static method Second<T>(a: T, o: Object?): bool => a.Equals(o); "
    "  public static method Hash<T>(a: T): int => a.GetHashCode(); ";

std::string programWith(const std::string& body) {
    return std::string(kGenerics) +
        "  public static method Main(): int " + body +
        "    return 0; end Main; end C; ";
}

// ASan/LSan oracle (the default).
JitResult runMain(const std::string& body) {
    return runAotChecked(programWith(body));
}

// runAot, not runAotChecked: boxed Object? argument leak, #5951 — convert to
// runAotChecked when #5951 is fixed. A boxed value handed to a GENERIC callee's
// `Object?` parameter leaks its call-site box under LSan whatever the
// instantiation (measured at `Second<int>(a, b)` and `Via<int>` on r30-c1, no
// inline array). The rows' observable is the printed result; the rows without
// a boxed argument stay on runAotChecked above.
JitResult runMainNoLeakCheck(const std::string& body) {
    return runAot(programWith(body));
}

}  // namespace

// §9.2 first kind: Equals(T) at [3]int — equal contents true, one slot differs false.
BEZEL_TEST(generic_inline_array_universal_members_5806,
           first_kind_equals_t_element_wise_spec_9_2) {
    JitResult r = runMain(
        "var a: [3]int := [1, 2, 3]; var b: [3]int := [1, 2, 3]; "
        "var c: [3]int := [1, 2, 4]; "
        "Console.WriteLine(Same<[3]int>(a, b)); "
        "Console.WriteLine(Same<[3]int>(a, c)); ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\n"));
}

// §9.2 first kind at a string-element array.
BEZEL_TEST(generic_inline_array_universal_members_5806,
           first_kind_equals_t_string_elements_spec_9_2) {
    JitResult r = runMain(
        "var a: [2]string := [\"x\", \"y\"]; var b: [2]string := [\"x\", \"y\"]; "
        "var c: [2]string := [\"x\", \"z\"]; "
        "Console.WriteLine(Same<[2]string>(a, b)); "
        "Console.WriteLine(Same<[2]string>(a, c)); ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\n"));
}

// §9.2 second kind: Equals(Object?) — a boxed [3]int of the same type compares
// element-wise (true and false), `null` and another array type compare unequal.
// (A boxed `int` literal argument is not used: the call-site box of a value
// literal at an `Object?` parameter leaks independently of this fix, also at
// `Second<int>(7, 5)`, which would trip runAotChecked's LSan.)
BEZEL_TEST(generic_inline_array_universal_members_5806,
           second_kind_equals_object_boxed_rule_spec_9_2) {
    JitResult r = runMainNoLeakCheck(
        "var a: [3]int := [1, 2, 3]; var b: [3]int := [1, 2, 3]; "
        "var c: [3]int := [1, 2, 4]; "
        "Console.WriteLine(Second<[3]int>(a, b)); "
        "Console.WriteLine(Second<[3]int>(a, c)); "
        "Console.WriteLine(Second<[3]int>(a, null)); ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nFalse\n"));
}

// Second kind with a `null` argument under the ASan/LSan oracle: no argument
// box exists, so this exercises the receiver box's mint and release in the
// instantiation (a double release or a leak of the receiver box fails here).
BEZEL_TEST(generic_inline_array_universal_members_5806,
           second_kind_null_argument_receiver_box_released_spec_9_2) {
    JitResult r = runMain(
        "var a: [3]int := [1, 2, 3]; "
        "Console.WriteLine(Second<[3]int>(a, null)); "
        "Console.WriteLine(Second<[3]int>(a, null)); ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nFalse\n"));
}

// Second kind, a boxed inline array of a DIFFERENT type ([4]int vs [3]int)
// compares unequal (the box descriptors differ). Own row: two array-box call
// arguments of different sizes in one body trip an independent call-site box
// leak under runAotChecked's LSan (measured, not caused by this lowering).
BEZEL_TEST(generic_inline_array_universal_members_5806,
           second_kind_other_array_type_unequal_spec_9_2) {
    JitResult r = runMainNoLeakCheck(
        "var a: [3]int := [1, 2, 3]; var d: [4]int := [1, 2, 3, 4]; "
        "Console.WriteLine(Second<[3]int>(a, d)); ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\n"));
}

// §9.2 GetHashCode + §8.3 "if a.Equals(b) then a.GetHashCode() = b.GetHashCode()":
// equal arrays hash equal, and the hash is not a constant (a differing array
// hashes differently, 17*31^3 + element fold).
BEZEL_TEST(generic_inline_array_universal_members_5806,
           get_hash_code_element_wise_spec_9_2) {
    JitResult r = runMain(
        "var a: [3]int := [1, 2, 3]; var b: [3]int := [1, 2, 3]; "
        "var c: [3]int := [1, 2, 4]; "
        "var ha := Hash<[3]int>(a); var hb := Hash<[3]int>(b); "
        "var hc := Hash<[3]int>(c); "
        "Console.WriteLine(ha = hb); "
        "Console.WriteLine(ha = hc); "
        "Console.WriteLine(ha = a.GetHashCode()); ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nTrue\n"));
}

// Same second-kind rule on the directly-typed (non-generic) receiver: the
// shared lowering (tryLowerInlineArrayCall) must agree with the generic one.
BEZEL_TEST(generic_inline_array_universal_members_5806,
           second_kind_direct_inline_array_receiver_spec_8_3) {
    JitResult r = runMainNoLeakCheck(
        "var a: [3]int := [1, 2, 3]; var b: [3]int := [1, 2, 3]; "
        "var c: [3]int := [1, 2, 4]; "
        "Console.WriteLine(Second<[3]int>(a, b) = a.Equals(b)); "
        "var n: Object? := null; "
        "Console.WriteLine(a.Equals(n)); "
        "Console.WriteLine(a.Equals(c)); ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nFalse\n"));
}

// Controls: the already-working instantiations keep their answers.
BEZEL_TEST(generic_inline_array_universal_members_5806,
           control_int_instantiation_unchanged_spec_9_2) {
    JitResult r = runMain(
        "Console.WriteLine(Same<int>(5, 5)); "
        "Console.WriteLine(Same<int>(5, 6)); "
        "Console.WriteLine(Hash<int>(7) = Hash<int>(7)); ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nTrue\n"));
}

// The cell this commit does NOT lower: `a.GetHashCode()` on `T := int?` stays a
// hard compile ERROR (never a silent constant), and its diagnostic names the
// issue that tracks it (#5953), not the closed #2457.
BEZEL_TEST(generic_inline_array_universal_members_5806,
           remaining_nullable_primitive_get_hash_code_is_loud_and_tracked) {
    JitResult r = runAot(programWith(
        "var a: int? := 5; Console.WriteLine(Hash<int?>(a) = 5); "));
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("member `GetHashCode` on `T` is declared via the "
                           "universal Object contract") != std::string::npos &&
            d.message.find("(tracked #5953)") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}
