// fix #8174 (§14.1, §2.7 rule 1, v0.355): a BUILT-IN type reached from
// another declaration inside a generic body whose type parameter is spelled
// like that built-in must keep the built-in's type.
//
// THE DEFECT. Inside `class Box<Guid>` instantiated at `Box<int>`, a read of
// `h.G` where `Holder.G` is declared `Guid` (the built-in, in a declaration
// where no type parameter is in scope) was lowered through Box's active
// substitution `Guid -> int` BY NAME: `lowerTypeRef`, and every receiver /
// argument / element site that asked `genericSubst_` or `genericSubstSemaTy_`
// for the TypeRef's head name, retyped the 16-byte `%bz.guid` as a 4-byte
// `int`. The program compiled and printed `1` (the issue's repro) where the
// `class Box<T>` twin prints `36`; an element or key read crashed; at
// `Box<string>` the IR did not verify.
//
// THE SPEC. §14.1 *Type-parameter names* (v0.355): "Type parameters of
// declarations that do not nest share no scope". §2.7 rule 1: "A type
// parameter is found at the link of the declaration that declares it".
// `Holder.G: Guid` is resolved in Holder's scope, where Box's parameter is not
// found, so it is the built-in `Guid` at every instantiation of Box; only a
// spelling the resolver bound to Box's parameter (`TypeRef::isResolvedTypeParam`,
// recorded by #6530) may take Box's substitution.
//
// Every row is on `runAotChecked` (generated-code ASan + LSan) and asserts the
// exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// stdout of a program that must compile, run and exit 0; a rejection, a crash
// or a non-zero exit reads back as a marker no expectation equals.
std::string out(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// The issue's literal repro, with Box's parameter spelled `param`.
std::string issueRepro(const std::string& param) {
    return "module P;\n"
           "public class Holder\n"
           "  public property G: Guid;\n"
           "  public constructor()\n"
           "    G := Guid.NewGuid();\n"
           "  end constructor;\n"
           "end Holder;\n"
           "public class Box<" + param + ">\n"
           "  public method Go(h: Holder): int\n"
           "    var s := h.G.ToString();\n"
           "    return s.Length;\n"
           "  end Go;\n"
           "end Box;\n"
           "public class Program\n"
           "  public static method Main()\n"
           "    var h := new Holder();\n"
           "    var b := new Box<int>();\n"
           "    Console.WriteLine(b.Go(h));\n"
           "  end Main;\n"
           "end Program;\n";
}

// A Holder declaring built-in `Guid` in every position the other spellings
// read: a property, a method return, an owned `Array<Guid>` and an owned
// `Dictionary<Guid, int>`; and a non-generic helper taking a `Guid`.
const char* kHolder =
    "module P;\n"
    "public class Holder\n"
    "  public property G: Guid;\n"
    "  public owned A: Array<Guid>;\n"
    "  public owned D: Dictionary<Guid, int>;\n"
    "  public constructor()\n"
    "    G := Guid.NewGuid();\n"
    "    A := new Array<Guid>();\n"
    "    A.Add(G);\n"
    "    D := new Dictionary<Guid, int>();\n"
    "    D[G] := 7;\n"
    "  end constructor;\n"
    "  public method Get(): Guid\n"
    "    return G;\n"
    "  end Get;\n"
    "end Holder;\n"
    "public class Util\n"
    "  public static method Len(g: Guid): int\n"
    "    return g.ToString().Length;\n"
    "  end Len;\n"
    "end Util;\n";

// `class Box<Guid>` whose `Go` returns `body`, instantiated at `Box<arg>`.
std::string boxProgram(const std::string& body, const std::string& arg) {
    return std::string(kHolder) +
           "public class Box<Guid>\n"
           "  public method Go(h: Holder): int\n" + body +
           "  end Go;\n"
           "end Box;\n"
           "public class Program\n"
           "  public static method Main()\n"
           "    using var h := new Holder();\n"
           "    var b := new Box<" + arg + ">();\n"
           "    Console.WriteLine(b.Go(h));\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

// The issue's literal repro. Pre-fix: `1`.
BEZEL_TEST(builtin_named_type_param_capture_8174,
           issue_repro_holder_guid_read_inside_box_guid_at_int_spec_14_1) {
    BEZEL_EXPECT_EQ(std::string("36\n"), out(issueRepro("Guid")));
}

// The spelling control from the issue: Box's parameter named `T`. Green on
// every compiler; only the parameter's spelling separated it from the row
// above.
BEZEL_TEST(builtin_named_type_param_capture_8174,
           control_issue_repro_box_param_named_T) {
    BEZEL_EXPECT_EQ(std::string("36\n"), out(issueRepro("T")));
}

// Repeated: 200 instantiations' reads summed (a stable value per read).
BEZEL_TEST(builtin_named_type_param_capture_8174,
           loop_holder_guid_read_inside_box_guid_200x_spec_14_1) {
    BEZEL_EXPECT_EQ(
        std::string("7200\n"),
        out("module P;\n"
            "public class Holder\n"
            "  public property G: Guid;\n"
            "  public constructor()\n"
            "    G := Guid.NewGuid();\n"
            "  end constructor;\n"
            "end Holder;\n"
            "public class Box<Guid>\n"
            "  public method Go(h: Holder): int\n"
            "    return h.G.ToString().Length;\n"
            "  end Go;\n"
            "end Box;\n"
            "public class Program\n"
            "  public static method Main()\n"
            "    var total := 0;\n"
            "    for var i in 0..<200 do\n"
            "      var h := new Holder();\n"
            "      var b := new Box<int>();\n"
            "      total := total + b.Go(h);\n"
            "    end;\n"
            "    Console.WriteLine(total);\n"
            "  end Main;\n"
            "end Program;\n"));
}

// Spelling: a local inferred from the built-in field. Pre-fix: `1`.
BEZEL_TEST(builtin_named_type_param_capture_8174,
           inferred_local_from_builtin_field_spec_14_1) {
    BEZEL_EXPECT_EQ(std::string("36\n"),
                    out(boxProgram("    var g := h.G;\n"
                                   "    return g.ToString().Length;\n",
                                   "int")));
}

// Spelling: another declaration's method returning `Guid`. Pre-fix: `1`.
BEZEL_TEST(builtin_named_type_param_capture_8174,
           builtin_method_return_spec_14_1) {
    BEZEL_EXPECT_EQ(std::string("36\n"),
                    out(boxProgram("    return h.Get().ToString().Length;\n",
                                   "int")));
}

// Spelling: the built-in passed to another declaration's `Guid` parameter.
// Pre-fix: `1`.
BEZEL_TEST(builtin_named_type_param_capture_8174,
           builtin_field_as_argument_to_builtin_parameter_spec_14_1) {
    BEZEL_EXPECT_EQ(std::string("36\n"),
                    out(boxProgram("    return Util.Len(h.G);\n", "int")));
}

// Spelling: the element of another declaration's `Array<Guid>`. Pre-fix: a
// crash.
BEZEL_TEST(builtin_named_type_param_capture_8174,
           builtin_array_element_spec_14_1) {
    BEZEL_EXPECT_EQ(std::string("36\n"),
                    out(boxProgram("    return h.A[0].ToString().Length;\n",
                                   "int")));
}

// Spelling: another declaration's `Dictionary<Guid, int>` indexed by the
// built-in field. Pre-fix: a crash.
BEZEL_TEST(builtin_named_type_param_capture_8174,
           builtin_dictionary_key_spec_14_1) {
    BEZEL_EXPECT_EQ(std::string("7\n"),
                    out(boxProgram("    return h.D[h.G];\n", "int")));
}

// Spelling: a lambda in Box's body reading the built-in field. Pre-fix: `1`.
// The lambda takes the Holder as its own parameter rather than capturing `h`:
// a capture cell inside a generic class's method leaks independently of the
// spelling (#5228, the `T` twin leaks the same 8 bytes), which would fail the
// LSan leg for a reason this row does not test.
BEZEL_TEST(builtin_named_type_param_capture_8174,
           builtin_field_read_in_lambda_spec_14_1) {
    BEZEL_EXPECT_EQ(
        std::string("36\n"),
        out(boxProgram(
            "    var f := lambda(x: Holder) => x.G.ToString().Length;\n"
            "    return f(h);\n",
            "int")));
}

// Spelling: the issue's read at `Box<string>`. Pre-fix: the IR did not
// verify (a `%bz.guid` value handed to the string lowering).
BEZEL_TEST(builtin_named_type_param_capture_8174,
           builtin_field_read_inside_box_guid_at_string_spec_14_1) {
    BEZEL_EXPECT_EQ(
        std::string("36\n"),
        out(boxProgram("    return h.G.ToString().Length;\n", "string")));
}

// Spelling: a generic METHOD's parameter named `Guid` (the substitution of a
// method instantiation, not a class's). Pre-fix: `1`.
BEZEL_TEST(builtin_named_type_param_capture_8174,
           generic_method_param_named_guid_reads_builtin_field_spec_14_1) {
    BEZEL_EXPECT_EQ(
        std::string("36\n4\n"),
        out(std::string(kHolder) +
            "public class C\n"
            "  public method Go<Guid>(h: Holder, x: Guid): Guid\n"
            "    Console.WriteLine(h.G.ToString().Length);\n"
            "    return x;\n"
            "  end Go;\n"
            "end C;\n"
            "public class Program\n"
            "  public static method Main()\n"
            "    using var h := new Holder();\n"
            "    var c := new C();\n"
            "    Console.WriteLine(c.Go<int>(h, 3) + 1);\n"
            "  end Main;\n"
            "end Program;\n"));
}

// Control: inside the same `Box<Guid>`, the PARAMETER `Guid` (field,
// constructor parameter, local annotation, return type) still takes the
// substitution at `Box<int>` and `Box<string>`, beside a built-in read.
// Pre-fix the built-in reads printed `1` and the `Box<string>` half did not
// compile.
BEZEL_TEST(builtin_named_type_param_capture_8174,
           control_param_named_guid_still_substitutes_beside_builtin_read) {
    BEZEL_EXPECT_EQ(
        std::string("36\n42\n36\n2\n"),
        out(std::string(kHolder) +
            "public class Box<Guid>\n"
            "  public property V: Guid;\n"
            "  public constructor(v: Guid)\n"
            "    V := v;\n"
            "  end constructor;\n"
            "  public method Go(h: Holder): Guid\n"
            "    var t: Guid := V;\n"
            "    Console.WriteLine(h.G.ToString().Length);\n"
            "    return t;\n"
            "  end Go;\n"
            "end Box;\n"
            "public class Program\n"
            "  public static method Main()\n"
            "    using var h := new Holder();\n"
            "    var b := new Box<int>(41);\n"
            "    Console.WriteLine(b.Go(h) + 1);\n"
            "    var s := new Box<string>(\"ab\");\n"
            "    Console.WriteLine(s.Go(h).Length);\n"
            "  end Main;\n"
            "end Program;\n"));
}

// Control: the built-in read in a NON-generic class (no parameter to shadow
// it). Green on every compiler.
BEZEL_TEST(builtin_named_type_param_capture_8174,
           control_builtin_read_in_non_generic_class) {
    BEZEL_EXPECT_EQ(
        std::string("36\n7\n"),
        out(std::string(kHolder) +
            "public class Plain\n"
            "  public method Go(h: Holder): int\n"
            "    Console.WriteLine(h.A[0].ToString().Length);\n"
            "    return h.D[h.Get()];\n"
            "  end Go;\n"
            "end Plain;\n"
            "public class Program\n"
            "  public static method Main()\n"
            "    using var h := new Holder();\n"
            "    var p := new Plain();\n"
            "    Console.WriteLine(p.Go(h));\n"
            "  end Main;\n"
            "end Program;\n"));
}
