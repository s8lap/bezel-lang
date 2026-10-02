// Codegen — #5249 (spec §14.1, v0.313): a class type parameter shadows a
// same-named type inside the class body. `class Box<Stopwatch>` with a field
// `v: Stopwatch` instantiated at `int` has an `int` field; the destructor's
// field-release walk used to classify the field by the bare NAME (builtin
// Stopwatch) before the generic substitution, so Box<int>'s disposal called
// the Stopwatch free on an int (`free(): invalid pointer`, rc 134).
// Reverse direction of #5241 (builtin captured by a same-named parameter).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

// R1: the issue repro, constructed 200x so a bad free shows.
BEZEL_TEST(type_param_named_like_builtin_5249, spec_14_1_type_param_named_stopwatch_int_field_not_freed) {
    auto r = runAot(
        "module M; "
        "class Box<Stopwatch> "
        "  public v: Stopwatch; "
        "  public constructor(x: Stopwatch) v := x; end constructor; "
        "end Box; "
        "class C public static method Main() "
        "  var s := 0; "
        "  for var i in 0..199 do "
        "    var b := new Box<int>(5); "
        "    s := s + b.v; "
        "  end for; "
        "  Console.WriteLine(s); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran, true);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n"));
}

// R2b (control, passes before and after): parameters named like other vtableless / collection builtins,
// instantiated at an owned user class: disposed exactly once, no leak.
BEZEL_TEST(type_param_named_like_builtin_5249, spec_14_1_type_param_named_builtin_owned_class_field_no_leak) {
    for (const char* n : {"Stack", "StringBuilder", "Mutex", "Stopwatch"}) {
        std::string src =
            "module M; "
            "class Item public x: int; "
            "  public constructor(a: int) x := a; end constructor; end Item; "
            "class Box<" + std::string(n) + "> "
            "  public owned v: " + n + "; "
            "  public constructor(owned x: " + n + ") v := x; end constructor; "
            "end Box; "
            "class C public static method Main() "
            "  var s := 0; "
            "  for var i in 0..199 do "
            "    var b := new Box<Item>(new Item(i)); "
            "    s := s + b.v.x; "
            "  end for; "
            "  Console.WriteLine(s); "
            "end Main; end C;";
        auto r = runAotChecked(src);
        BEZEL_EXPECT_EQ(r.ran, true);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("19900\n"));
    }
}

// R2: the same walk for the other vtableless builtin names (StringBuilder,
// Random, Mutex, Lock, Channel), each instantiated at `int` (non-owned field).
BEZEL_TEST(type_param_named_like_builtin_5249, spec_14_1_type_param_named_other_builtins_int_field_not_freed) {
    for (const char* n : {"StringBuilder", "Random", "Mutex", "Lock", "Channel"}) {
        std::string src =
            "module M; "
            "class Box<" + std::string(n) + "> "
            "  public v: " + n + "; "
            "  public constructor(x: " + n + ") v := x; end constructor; "
            "end Box; "
            "class C public static method Main() "
            "  var s := 0; "
            "  for var i in 0..199 do "
            "    var b := new Box<int>(5); "
            "    s := s + b.v; "
            "  end for; "
            "  Console.WriteLine(s); "
            "end Main; end C;";
        auto r = runAot(src);
        BEZEL_EXPECT_EQ(r.ran, true);
        BEZEL_EXPECT_EQ(r.exitCode, 0);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("1000\n"));
    }
}

// R4 (control): the parameter named T.
BEZEL_TEST(type_param_named_like_builtin_5249, spec_14_1_control_type_param_named_t) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class Box<T> "
        "  public v: T; "
        "  public constructor(x: T) v := x; end constructor; "
        "end Box; "
        "class C public static method Main() "
        "  for var i in 0..199 do var b := new Box<int>(5); end for; "
        "  var b := new Box<int>(5); "
        "  Console.WriteLine(b.v); "
        "end Main; end C;"), "5");
}
