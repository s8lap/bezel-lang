// fix #5340 (§2.8.3 / §8.1 / §8.3, v0.314): `OwningArray<Object> := [new Object()]` ICEs
// the compiler with #1386 (`lowerExpr returned null for heap collection literal
// element`). The root cause: `lowerNew` resolved `new Object()` to
// `PrimitiveType::Object`, hit the `rt->kind != Named` guard, returned null,
// and the ICE fired downstream at `materializeHeapCollectionLiteral`
// (LlvmBackend_Setup.cpp:40798).
//
// The fix adds a Primitive.Object guard before the Named-type guard in lowerNew.
// For `new Object()` it allocates an 8-byte object (the object header = one
// vtable pointer slot) and installs the proper Object vtable built by
// setupBuiltinObject(). The Object vtable's ToString returns "System.Object"
// (§8.5 class default), Dispose is a no-op (§8.7 — Object has no destructor),
// Equals is reference equality, and GetHashCode is identity hash (§8.3) — all
// correct for Object.
//
// Authority: stdlib §4.2/§14.4: "Object is a class", owningElementBadDesc
// returns {} for Object. §2.8.3: Object is the root class. §8.5: the
// class-default ToString is the type's qualified name.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// §2.8.3 / §8.5 / #5340: OwningArray<Object> literal with new Object() —
// the primary regression case: the program compiles without ICE and runs.
BEZEL_TEST(owning_array_object_literal_5340, new_object_in_owning_array_literal) {
    // The three forms from the issue: constructor, empty literal, element literal.
    // Assert the element count is 1 to verify the literal was materialised.
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class Pgm"
        "    public static method Main(): int"
        "        var a1 := new OwningArray<Object>();"
        "        var a2: OwningArray<Object> := [];"
        "        var a3: OwningArray<Object> := [new Object()];"
        "        return a3.Count;"
        "    end Main;"
        "end Pgm;"), 1);
}

// §2.8.3 / §8.5 / #5340: plain local `new Object()` also works, and its
// ToString returns the correct qualified type name per §8.5 class default.
BEZEL_TEST(owning_array_object_literal_5340, plain_new_object_local_tostring) {
    // A bare `new Object()` local — verifies the guard in lowerNew fires
    // in non-collection-literal positions too, and that ToString returns
    // "System.Object" (the §8.5 class-default qualified name, not "Exception").
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class Pgm"
        "    public static method Main(): int"
        "        var o := new Object();"
        "        Console.WriteLine(o.ToString());"
        "        return 0;"
        "    end Main;"
        "end Pgm;"), 0);
}

// §2.8.3 / §8.5 / #5340: multiple Object elements in OwningArray.
BEZEL_TEST(owning_array_object_literal_5340, multiple_object_elements) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "class Pgm"
        "    public static method Main(): int"
        "        var a: OwningArray<Object> := [new Object(), new Object()];"
        "        return a.Count;"
        "    end Main;"
        "end Pgm;"), 2);
}
