// fix #6530 (§14.1 / §2.7 / §12, v0.355): the CODEGEN half — a type parameter
// named after a reserved builtin, carried through a base clause.
//
// THE DEFECT. `Emitter::computeAncestorGenericSubst` (the shared "which
// substitution is an ancestor reached under" authority, #585/#2840) resolves
// each base-clause argument through the level below it. Its "is this argument
// a bare parameter of this level?" test was the NAME-keyed member
// `Emitter::isBareTypeParamRef` ("not a reserved builtin name, therefore a
// parameter"), so for
//
//     public class Base<K>  property D: Dictionary<K, int>; ...
//     public class Derived<Guid>: Base<Guid>  ...  D[0] := 11; return D[0];
//
// instantiated at `Derived<int>`, the argument `Guid` was read as the BUILT-IN
// and Base's `K` was bound to the bare `Guid` ref itself instead of to `int`.
// lowerTypeRef then sized the inherited dictionary's key at the 16-byte
// `%bz.guid` while the body stored and read a 4-byte `int` — the lookup threw
// KeyNotFoundException on a key it had just stored. The `T` twin printed 11.
// (Review r39-rev-6530b, probe a9. Until #6530's sema half, sema's own
// name-keyed over-rejection hid the miscompile behind a diagnostic.)
//
// THE SPEC. §14.1 *Type-parameter names* (v0.355): inside `Derived<Guid>`'s
// declaration the bare spelling `Guid` names the type parameter — "a type
// parameter has no qualified spelling", and the rule "does not reach a type
// parameter named like an ordinary type". The base clause `Base<Guid>` is
// resolved in Derived's scope, so its `Guid` IS Derived's parameter, and the
// instantiation `Derived<int>` makes Base's `K` `int` (§3.6 rule 2: the body
// is checked, and here laid out, per instantiation after substitution). The
// only sound answer to "is this spelling a parameter here" is the level's
// DECLARED parameter list.
//
// Every row is on `runAotChecked` (generated-code ASan + LSan): a wrong key
// size is a layout defect, and the exact printed value is the oracle.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// stdout of a program that must compile, run and exit 0. A rejection or a
// non-zero exit reads back as a marker no expectation equals, so the pre-fix
// over-rejection (sema) and the pre-fix KeyNotFoundException (exit 70) both
// FAIL an accept row rather than passing it.
std::string out(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// `Base<K>` owns a `Dictionary<K, int>` built in its own constructor, so the
// key's byte size is decided where Base's body is inlined under the ancestor
// substitution the derived instantiation induces. The field is `owned`
// (§28.5) so the dictionary is disposed with the instance and the LSan leg
// is a real oracle (the review's probe a9 held it in a non-owning property,
// which LSan reports as leaked on EVERY spelling, `T` included).
const char* kBase =
    "module P;\n"
    "public class Base<K>\n"
    "  public owned D: Dictionary<K, int>;\n"
    "  public constructor()\n"
    "    D := new Dictionary<K, int>();\n"
    "  end constructor;\n"
    "end Base;\n";

// A derived class whose parameter is spelled `P`, deriving `Base<P>`, with a
// body that stores and reads the inherited dictionary at `key`.
std::string derived(const std::string& param, const std::string& key) {
    return "public class Derived<" + param + ">: Base<" + param + ">\n"
           "  public constructor()\n"
           "    base();\n"
           "  end constructor;\n"
           "  public method Go(): int\n"
           "    D[" + key + "] := 11;\n"
           "    return D[" + key + "];\n"
           "  end Go;\n"
           "end Derived;\n";
}

std::string program(const std::string& arg) {
    return "public class Program\n"
           "  public static method Main()\n"
           "    using var x := new Derived<" + arg + ">();\n"
           "    Console.WriteLine(x.Go());\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

// The review's probe a9, verbatim in shape: parameter `Guid`, instantiated at
// `int`. Pre-fix: rejected by sema's name-keyed guard; with only the sema half
// fixed: compiled, then KeyNotFoundException (exit 70).
BEZEL_TEST(inherited_generic_key_6530,
           derived_param_named_guid_at_int_reads_back_stored_key_spec_14_1) {
    BEZEL_EXPECT_EQ(std::string("11\n"),
                    out(std::string(kBase) + derived("Guid", "0") +
                        program("int")));
}

// The spelling control: the same program with the parameter named `T`. Green
// on every compiler; with the row above it shows that only the parameter's
// SPELLING separated a correct program from a miscompiled one.
BEZEL_TEST(inherited_generic_key_6530,
           control_derived_param_named_T_at_int_reads_back_stored_key) {
    BEZEL_EXPECT_EQ(std::string("11\n"),
                    out(std::string(kBase) + derived("T", "0") +
                        program("int")));
}

// `Range` is the other reserved builtin name the issue measures; its builtin
// lowering is a different (non-16-byte) shape, so a second spelling pins that
// the predicate is the declared list, not a Guid special case.
BEZEL_TEST(inherited_generic_key_6530,
           derived_param_named_range_at_int_reads_back_stored_key) {
    BEZEL_EXPECT_EQ(std::string("11\n"),
                    out(std::string(kBase) + derived("Range", "0") +
                        program("int")));
}

// At `long` the correct key is 8 bytes and the builtin reading is 16: neither
// coincides with the 4-byte erased placeholder, so a fix that merely fell
// back to the template layout would also fail this row.
BEZEL_TEST(inherited_generic_key_6530,
           derived_param_named_guid_at_long_reads_back_stored_key) {
    BEZEL_EXPECT_EQ(std::string("11\n"),
                    out(std::string(kBase) + derived("Guid", "5L") +
                        program("long")));
}

// At `string` the dictionary takes the string fast path (bz_dict_alloc, not the
// generic-key variant): the substitution must reach the key-kind decision as
// well as the byte size.
BEZEL_TEST(inherited_generic_key_6530,
           derived_param_named_guid_at_string_reads_back_stored_key) {
    BEZEL_EXPECT_EQ(std::string("11\n"),
                    out(std::string(kBase) + derived("Guid", "\"k\"") +
                        program("string")));
}

// A two-link chain: `Leaf<Guid>: Mid<Guid>`, `Mid<Guid>: Base<Guid>`. Every
// level's bare `Guid` is THAT level's own parameter, resolved through the
// level below — the walk must ask each level's declared list, not the first's.
BEZEL_TEST(inherited_generic_key_6530,
           two_level_chain_param_named_guid_at_int_reads_back_stored_key) {
    BEZEL_EXPECT_EQ(
        std::string("11\n"),
        out(std::string(kBase) +
            "public class Mid<Guid>: Base<Guid>\n"
            "  public constructor()\n"
            "    base();\n"
            "  end constructor;\n"
            "end Mid;\n"
            "public class Leaf<Guid>: Mid<Guid>\n"
            "  public constructor()\n"
            "    base();\n"
            "  end constructor;\n"
            "  public method Go(): int\n"
            "    D[0] := 11;\n"
            "    return D[0];\n"
            "  end Go;\n"
            "end Leaf;\n"
            "public class Program\n"
            "  public static method Main()\n"
            "    using var x := new Leaf<int>();\n"
            "    Console.WriteLine(x.Go());\n"
            "  end Main;\n"
            "end Program;\n"));
}

// The admitting twin for the BUILTIN direction: a NON-generic derived class
// `class D: Base<Guid>` declares no parameter, so its `Guid` is stdlib §2.6's
// built-in, and the inherited dictionary must keep the 16-byte Guid key — a
// real `Guid` key stored and read back. Green on every compiler: it shows the
// declared-list test does not capture a built-in from a level that declares
// no such parameter.
BEZEL_TEST(inherited_generic_key_6530,
           control_non_generic_derived_keeps_builtin_guid_key) {
    BEZEL_EXPECT_EQ(
        std::string("11\n"),
        out(std::string(kBase) +
            "public class D: Base<Guid>\n"
            "  public constructor()\n"
            "    base();\n"
            "  end constructor;\n"
            "  public method Go(): int\n"
            "    var g := Guid.NewGuid();\n"
            "    D[g] := 11;\n"
            "    return D[g];\n"
            "  end Go;\n"
            "end D;\n"
            "public class Program\n"
            "  public static method Main()\n"
            "    using var x := new D();\n"
            "    Console.WriteLine(x.Go());\n"
            "  end Main;\n"
            "end Program;\n"));
}

// The parameter named `Guid`, instantiated at the BUILT-IN `Guid` from a
// scope that declares no such parameter (`Program`): `Derived<Guid>` there
// means `Derived<System.Guid>`, so Base's `K` is the real 16-byte Guid and a
// real Guid key round-trips. Pins that the two readings coexist in one program.
BEZEL_TEST(inherited_generic_key_6530,
           derived_param_named_guid_at_builtin_guid_reads_back_stored_key) {
    BEZEL_EXPECT_EQ(
        std::string("11\n"),
        out(std::string(kBase) +
            "public class Derived<Guid>: Base<Guid>\n"
            "  public constructor()\n"
            "    base();\n"
            "  end constructor;\n"
            "  public method Go(k: Guid): int\n"
            "    D[k] := 11;\n"
            "    return D[k];\n"
            "  end Go;\n"
            "end Derived;\n"
            "public class Program\n"
            "  public static method Main()\n"
            "    using var x := new Derived<Guid>();\n"
            "    Console.WriteLine(x.Go(Guid.NewGuid()));\n"
            "  end Main;\n"
            "end Program;\n"));
}
