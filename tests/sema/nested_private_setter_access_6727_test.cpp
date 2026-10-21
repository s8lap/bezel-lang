// Sema — #6727 / spec §11.6 + §13.1 (v0.340), §13.6 accessor visibility.
//
// §11.6: the enclosing class has full access to all members (`private`
// included) of every type nested in it, at any depth, and a nested type has
// full access to its enclosing types' members; §13.1: `private` is lexical as
// well as nominal, so two siblings nested in one class have no access to each
// other's `private` members. The property-setter visibility paths (plain
// assignment / compound assignment and the `ref`/`out` operand check) lacked
// the nesting clause `checkMemberVisibility` has had since #954, so a nested
// type's `private set` was rejected from the enclosing body (and the mirror)
// while a `private` field write was admitted. Accept rows run through runAot
// with exact stdout; reject rows pin the exact §13.6 diagnostic with a real
// location.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotModules;

namespace {

// Enclosing -> nested: instance (auto + block, `private`/`protected`/
// `internal` set), static, plain and compound assignment, and `this.`-qualified
// and bare self writes inside the nested body.
constexpr const char* kEnclosingToNested =
    "module M;\n"
    "public class Outer\n"
    "  public class Inner\n"
    "    public property Q: int private set;\n"
    "    public property R: int protected set;\n"
    "    public property T: int internal set;\n"
    "    private _v: int := 0;\n"
    "    public property V: int\n"
    "      get => _v;\n"
    "      private set => do _v := value; end;\n"
    "    end V;\n"
    "    public static property S: int private set;\n"
    "    public method Bump() this.Q := this.Q + 10; Q += 1; V := V + 1; end Bump;\n"
    "    destructor end destructor;\n"
    "  end Inner;\n"
    "  public method SetIt(i: Inner)\n"
    "    i.Q := 100; i.Q += 5; i.R := 3; i.T := 4; i.V := 9;\n"
    "    Inner.S := 7; Inner.S += 1;\n"
    "  end SetIt;\n"
    "  public static method Main(): int\n"
    "    using var o := new Outer();\n"
    "    using var i := new Inner();\n"
    "    o.SetIt(i); i.Bump();\n"
    "    Console.WriteLine(i.Q); Console.WriteLine(i.R); Console.WriteLine(i.T);\n"
    "    Console.WriteLine(i.V); Console.WriteLine(Inner.S);\n"
    "    return 0;\n"
    "  end Main;\n"
    "  destructor end destructor;\n"
    "end Outer;\n";

// Nested (two levels deep) -> enclosing, and middle -> enclosing / enclosing
// -> middle; instance and static.
constexpr const char* kNestedToEnclosing =
    "module M;\n"
    "public class Outer\n"
    "  public property OP: int private set;\n"
    "  public static property OS: int private set;\n"
    "  public class Mid\n"
    "    public class Deep\n"
    "      public method Poke(o: Outer, m: Mid)\n"
    "        o.OP := 11; o.OP += 1; Outer.OS := 21; m.MP := 31;\n"
    "      end Poke;\n"
    "      destructor end destructor;\n"
    "    end Deep;\n"
    "    public property MP: int private set;\n"
    "    public method PokeOuter(o: Outer) o.OP := 5; Outer.OS += 2; end PokeOuter;\n"
    "    destructor end destructor;\n"
    "  end Mid;\n"
    "  public static method Main(): int\n"
    "    using var o := new Outer();\n"
    "    using var m := new Mid();\n"
    "    using var d := new Mid.Deep();\n"
    "    d.Poke(o, m);\n"
    "    Console.WriteLine(o.OP); Console.WriteLine(Outer.OS); Console.WriteLine(m.MP);\n"
    "    m.PokeOuter(o);\n"
    "    Console.WriteLine(o.OP); Console.WriteLine(Outer.OS);\n"
    "    return 0;\n"
    "  end Main;\n"
    "  destructor end destructor;\n"
    "end Outer;\n";

// `out` operand of a nested type's `private set` property from the enclosing
// body (the checkRefOutPropertyOperand path).
constexpr const char* kOutOperand =
    "module M;\n"
    "public class Outer\n"
    "  public class Inner\n"
    "    public property Q: int private set;\n"
    "    destructor end destructor;\n"
    "  end Inner;\n"
    "  public static method Fill(out n: int) n := 42; end Fill;\n"
    "  public static method Main(): int\n"
    "    using var i := new Inner();\n"
    "    Fill(out i.Q);\n"
    "    Console.WriteLine(i.Q);\n"
    "    return 0;\n"
    "  end Main;\n"
    "  destructor end destructor;\n"
    "end Outer;\n";

constexpr const char* kSiblings =
    "module M;\n"
    "public class Outer\n"
    "  public class A\n"
    "    public property P: int private set;\n"
    "    destructor end destructor;\n"
    "  end A;\n"
    "  public class B\n"
    "    public method Poke(a: A) a.P := 1; end Poke;\n"
    "    destructor end destructor;\n"
    "  end B;\n"
    "  public static method Main(): int return 0; end Main;\n"
    "  destructor end destructor;\n"
    "end Outer;\n";

constexpr const char* kSiblingsOut =
    "module M;\n"
    "public class Outer\n"
    "  public class A\n"
    "    public property P: int private set;\n"
    "    destructor end destructor;\n"
    "  end A;\n"
    "  public class B\n"
    "    public static method Fill(out n: int) n := 1; end Fill;\n"
    "    public method Poke(a: A) Fill(out a.P); end Poke;\n"
    "    destructor end destructor;\n"
    "  end B;\n"
    "  public static method Main(): int return 0; end Main;\n"
    "  destructor end destructor;\n"
    "end Outer;\n";

constexpr const char* kOutside =
    "module M;\n"
    "public class C\n"
    "  public property P: int private set;\n"
    "  destructor end destructor;\n"
    "end C;\n"
    "public class Other\n"
    "  public method Poke(c: C) c.P := 1; end Poke;\n"
    "  public static method Main(): int return 0; end Main;\n"
    "  destructor end destructor;\n"
    "end Other;\n";

// An outside class assigning a NESTED type's `private set` (nesting does not
// open it to a type that merely sits in the same module).
constexpr const char* kOutsideToNested =
    "module M;\n"
    "public class Outer\n"
    "  public class Inner\n"
    "    public property P: int private set;\n"
    "    destructor end destructor;\n"
    "  end Inner;\n"
    "  destructor end destructor;\n"
    "end Outer;\n"
    "public class Other\n"
    "  public method Poke(i: Outer.Inner) i.P := 1; end Poke;\n"
    "  public static method Main(): int return 0; end Main;\n"
    "  destructor end destructor;\n"
    "end Other;\n";

// §26.3 (v0.340): "Extension members see only the extended type's public
// API"; an extension block has no declaring class, so §11.6's nesting grant
// does not reach a nested type's `private set` from an extension body even
// though the extension extends the enclosing type (reviewer probe p09).
constexpr const char* kExtensionToNested =
    "module M;\n"
    "public class Outer\n"
    "  public class Inner\n"
    "    public property P: int private set;\n"
    "    destructor end destructor;\n"
    "  end Inner;\n"
    "  destructor end destructor;\n"
    "end Outer;\n"
    "extension Outer\n"
    "  public method Poke(i: Outer.Inner)\n"
    "    i.P := 1;\n"
    "  end Poke;\n"
    "end;\n"
    "public class Entry\n"
    "  public static method Main(): int\n"
    "    using var o := new Outer();\n"
    "    using var i := new Outer.Inner();\n"
    "    o.Poke(i);\n"
    "    Console.WriteLine(i.P);\n"
    "    return 0;\n"
    "  end Main;\n"
    "  destructor end destructor;\n"
    "end Entry;\n";

// The `out`-operand twin (checkRefOutPropertyOperand).
constexpr const char* kExtensionToNestedOut =
    "module M;\n"
    "public class Outer\n"
    "  public class Inner\n"
    "    public property P: int private set;\n"
    "    destructor end destructor;\n"
    "  end Inner;\n"
    "  public static method Fill(out n: int) n := 1; end Fill;\n"
    "  destructor end destructor;\n"
    "end Outer;\n"
    "extension Outer\n"
    "  public method Poke(i: Outer.Inner)\n"
    "    Outer.Fill(out i.P);\n"
    "  end Poke;\n"
    "end;\n"
    "public class Entry\n"
    "  public static method Main(): int return 0; end Main;\n"
    "  destructor end destructor;\n"
    "end Entry;\n";

// §23.1 (v0.340): a `static readonly` auto-property is admitted "in the
// declaring type's own static initialization" only; an ENCLOSING type's
// instance constructor is not that, so the nesting grant must not open the
// nested type's `private` setter to it (reviewer probe p14b; the tip of the
// first #6727 commit accepted it and printed 5).
constexpr const char* kEnclosingCtorToNestedStaticReadonly =
    "module M;\n"
    "public class Outer\n"
    "  public class Inner\n"
    "    private static readonly property SR: int := 0;\n"
    "    public static method Peek(): int return SR; end Peek;\n"
    "    destructor end destructor;\n"
    "  end Inner;\n"
    "  public constructor()\n"
    "    Inner.SR := 5;\n"
    "  end constructor;\n"
    "  public static method Main(): int\n"
    "    using var o := new Outer();\n"
    "    Console.WriteLine(Inner.Peek());\n"
    "    return 0;\n"
    "  end Main;\n"
    "  destructor end destructor;\n"
    "end Outer;\n";

// The `out`-operand twin.
constexpr const char* kEnclosingCtorToNestedStaticReadonlyOut =
    "module M;\n"
    "public class Outer\n"
    "  public class Inner\n"
    "    private static readonly property SR: int := 0;\n"
    "    destructor end destructor;\n"
    "  end Inner;\n"
    "  public static method Fill(out n: int) n := 5; end Fill;\n"
    "  public constructor()\n"
    "    Fill(out Inner.SR);\n"
    "  end constructor;\n"
    "  public static method Main(): int return 0; end Main;\n"
    "  destructor end destructor;\n"
    "end Outer;\n";

// Pins the exact diagnostic at its real `<line>:<col>` (the location prefix
// the driver prints in a rejected compile's captured stderr).
void expectRejectedAt(const char* src, const char* locAndMessage) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, locAndMessage));
}

void expectRejected(const char* src) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "its setter is not accessible here"));
}

}  // namespace

BEZEL_TEST(nested_private_setter_access_6727, enclosing_to_nested_accepted_spec_11_6) {
    JitResult r = runAot(kEnclosingToNested);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("116\n3\n4\n10\n8\n"));
}

BEZEL_TEST(nested_private_setter_access_6727, nested_to_enclosing_and_deeper_accepted_spec_11_6) {
    JitResult r = runAot(kNestedToEnclosing);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("12\n21\n31\n5\n23\n"));
}

BEZEL_TEST(nested_private_setter_access_6727, enclosing_out_operand_of_nested_private_set_accepted_spec_11_6) {
    JitResult r = runAot(kOutOperand);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

BEZEL_TEST(nested_private_setter_access_6727, sibling_nested_assignment_rejected_spec_13_1) {
    expectRejected(kSiblings);
}

BEZEL_TEST(nested_private_setter_access_6727, sibling_nested_out_operand_rejected_spec_13_1) {
    JitResult r = runAot(kSiblingsOut);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "its setter is not accessible here"));
}

BEZEL_TEST(nested_private_setter_access_6727, outside_class_assignment_rejected_spec_13_6) {
    expectRejected(kOutside);
}

BEZEL_TEST(nested_private_setter_access_6727, outside_class_to_nested_rejected_spec_11_6) {
    expectRejected(kOutsideToNested);
}

// Cross-module: the importing module's class is not lexically nested in the
// library's, so the library's `private set` stays closed (§13.1, §13.6).
BEZEL_TEST(nested_private_setter_access_6727, cross_module_private_set_rejected_spec_13_6) {
    const char* lib =
        "module Lib;\n"
        "public class Outer\n"
        "  public class Inner\n"
        "    public property P: int private set;\n"
        "    public constructor() end constructor;\n"
        "    destructor end destructor;\n"
        "  end Inner;\n"
        "  destructor end destructor;\n"
        "end Outer;\n";
    const char* app =
        "module App; import Lib;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    using var i := new Outer.Inner();\n"
        "    i.P := 1;\n"
        "    return 0;\n"
        "  end Main;\n"
        "  destructor end destructor;\n"
        "end C;\n";
    JitResult r = runAotModules({ModuleSource{"Lib", lib}, ModuleSource{"App", app}}, "App");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "its setter is not accessible here"));
}

// §26.3 (v0.340): no nesting grant in an extension body.
BEZEL_TEST(nested_private_setter_access_6727, extension_body_to_nested_private_set_rejected_spec_26_3) {
    expectRejectedAt(kExtensionToNested,
                     ":11:5: cannot assign to property `P` — its setter is not "
                     "accessible here (§13.6)");
}

BEZEL_TEST(nested_private_setter_access_6727, extension_body_out_operand_of_nested_private_set_rejected_spec_26_3) {
    expectRejectedAt(kExtensionToNestedOut,
                     ":12:20: a `out` argument must be a property with an "
                     "accessible setter — its setter is not accessible here "
                     "(§22.3 / §13.6)");
}

// §23.1 (v0.340): no nesting grant for a `static readonly` write outside the
// declaring type's own static initialization.
BEZEL_TEST(nested_private_setter_access_6727, enclosing_ctor_to_nested_private_static_readonly_rejected_spec_23_1) {
    expectRejectedAt(kEnclosingCtorToNestedStaticReadonly,
                     ":9:5: cannot assign to property `SR` — its setter is not "
                     "accessible here (§13.6)");
}

BEZEL_TEST(nested_private_setter_access_6727, enclosing_ctor_out_operand_of_nested_private_static_readonly_rejected_spec_23_1) {
    expectRejectedAt(kEnclosingCtorToNestedStaticReadonlyOut,
                     ":9:14: a `out` argument must be a property with an "
                     "accessible setter — its setter is not accessible here "
                     "(§22.3 / §13.6)");
}
