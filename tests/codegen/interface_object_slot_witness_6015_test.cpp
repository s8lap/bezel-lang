// Regression tests for #6015 — a call through an INTERFACE member spelled as
// one of `Object`'s virtual members (`Equals(other: Object?)`, `GetHashCode()`,
// `ToString()`) on a boxed enum or choice.
//
// The box's interface table was filled by a name-and-arity lookup, which
// handed the kind's OWN-TYPED `Equals(other: T)` to a thunk that forwarded the
// caller's `Object?` argument unchanged: the argument's box POINTER arrived in
// a parameter lowered as the enum's integer or the choice's struct. An equal
// payload compared unequal, `null` compared EQUAL to the zero constant, and a
// choice body that read its argument matched on a garbage tag
// (`MatchFailureException`). A kind that declared no member at all got a NULL
// table entry, and the call jumped to address 0.
//
// Spec: §14.2 (v0.338), "Object's virtual members as witnesses": such an
// interface member "is matched ... by the member that fills the type's
// `Object` slot"; "on a receiver boxed to the interface the call reaches the
// witness through the box (§8.6) ... an argument that is not a boxed value of
// the type, `null` included, compares unequal". §9.2 (v0.338): the own-typed
// `Equals` is "the one member that fills the kind's `Object` slot", and an
// interface member at that signature "is satisfied by the kind's own member
// for that slot, declared or generated". §10.5 (enum), §35.7 (choice).
//
// Every declared `Equals` READS its argument (a key reduced mod 2 / mod 10),
// so two DIFFERENT values compare equal and no row can pass by a constant or
// by member-wise comparison. Rows loop 100x on the checked oracle and assert
// stdout: counters of how many iterations answered `true`.
//
// Arguments are read back out of a holder's `Object?` property and receivers
// are locals: those shapes release every box they make.
//
// NOT covered, each blocked elsewhere: a record through such an interface
// (sema still rejects the conformance, #5924 item 4), a generic choice (it
// does not box, #3905), and a `Set<IEq>` / `Dictionary<IEq, V>` key (the
// container reaches the same slot, but never releases its boxed keys, so no
// such row can run clean on the checked oracle).

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

const char* kPrelude =
    "module M;\n"
    "public interface IEq\n"
    "  method Equals(other: Object?): bool;\n"
    "  method GetHashCode(): int;\n"
    "end IEq;\n"
    "public interface IShow\n"
    "  method ToString(): string;\n"
    "end IShow;\n"
    "public class H\n"
    "  public property O: Object? := null;\n"
    "end H;\n"
    "public record Other(Q: int);\n";

// A = 0, B = 1, C = 2: declared equality is the ordinal mod 2, so A equals C.
const char* kEnum =
    "public enum E: IEq\n"
    "  A, B, C;\n"
    "  public method Equals(other: E): bool =>\n"
    "    (int(this) mod 2) = (int(other) mod 2);\n"
    "  public method GetHashCode(): int => (int(this) mod 2) + 40;\n"
    "end E;\n"
    // Same ordinals, another type: `F.A` is not an `E`.
    "public enum F\n  A, B, C;\nend F;\n";

// Declared equality is the payload mod 10; `Q` is its own key.
const char* kChoice =
    "public choice C: IEq\n"
    "  P(x: int), Q;\n"
    "  public method Key(): int => (case this of P(v) => v mod 10, Q => -1);\n"
    "  public method Equals(other: C): bool => Key() = other.Key();\n"
    "  public method GetHashCode(): int => Key() + 500;\n"
    "end C;\n"
    // Same shape, another type.
    "public choice D\n  P(x: int), Q;\nend D;\n";

// Nothing declared: the generated members are the witnesses (§9.2).
const char* kPlain =
    "public enum E: IEq, IShow\n  A, B, C;\nend E;\n"
    "public choice C: IEq, IShow\n  P(x: int), Q;\nend C;\n";

std::string countingProg(const std::string& decls, const std::string& setup,
                         const std::vector<std::string>& checks) {
    std::string s = std::string(kPrelude) + decls +
                    "public class Program\n"
                    "  public static method Main()\n";
    for (std::size_t i = 0; i < checks.size(); ++i)
        s += "    var n" + std::to_string(i) + " := 0;\n";
    s += "    for var j in 0..<100 do\n" + setup;
    for (std::size_t i = 0; i < checks.size(); ++i)
        s += "      n" + std::to_string(i) + " := n" + std::to_string(i) +
             " + (if " + checks[i] + " then 1 else 0);\n";
    s += "    end for;\n    Console.WriteLine(";
    for (std::size_t i = 0; i < checks.size(); ++i)
        s += std::string(i ? " + \" \" + " : "") + "n" + std::to_string(i) +
             ".ToString()";
    s += ");\n  end Main;\nend Program;\n";
    return s;
}

std::string holder(const char* name, const char* value) {
    return std::string("      using var ") + name + " := new H();\n" +
           (value ? std::string("      ") + name + ".O := " + value + ";\n"
                  : std::string());
}

}  // namespace

// #6015 (the issue's enum repro, with the `GetHashCode` §8.3 now requires
// beside it) / §10.5, §14.2 (v0.338): the interface call reaches the declared
// own-typed member on the argument's PAYLOAD. A vs C equal (mod 2), A vs B not.
BEZEL_TEST(interface_object_slot_witness_14_2_6015,
           enum_declared_equals_through_interface_reads_the_payload) {
    JitResult r = runAotChecked(countingProg(
        kEnum,
        "      var i: IEq := E.A;\n" +
            holder("hEq", "E.C") + holder("hSame", "E.A") +
            holder("hNe", "E.B"),
        {"i.Equals(hEq.O)", "i.Equals(hSame.O)", "i.Equals(hNe.O)"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 100 0\n"));
}

// §14.2 (v0.338): "an argument that is not a boxed value of the type, `null`
// included, compares unequal". The receiver is `E.A` (ordinal 0): the pre-fix
// thunk read `null` and every even pointer as ordinal 0 and answered TRUE.
BEZEL_TEST(interface_object_slot_witness_14_2_6015,
           enum_interface_equals_null_and_foreign_argument_is_unequal) {
    JitResult r = runAotChecked(countingProg(
        kEnum,
        "      var i: IEq := E.A;\n" +
            holder("hNull", nullptr) + holder("hInt", "0") +
            holder("hEnum", "F.A") + holder("hRec", "new Other(0)"),
        {"i.Equals(hNull.O)", "i.Equals(null)", "i.Equals(hInt.O)",
         "i.Equals(\"s\")", "i.Equals(hEnum.O)", "i.Equals(hRec.O)"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0 0 0 0 0 0\n"));
}

// §14.2 (v0.338): the declared `GetHashCode()` witnesses the interface member
// (unchanged by the fix; pinned so the Equals reroute cannot take it along).
BEZEL_TEST(interface_object_slot_witness_14_2_6015,
           enum_declared_hashcode_through_interface) {
    JitResult r = runAotChecked(countingProg(
        kEnum,
        "      var i: IEq := E.A;\n"
        "      var k: IEq := E.C;\n"
        "      var m: IEq := E.B;\n",
        {"i.GetHashCode() = 40", "i.GetHashCode() = k.GetHashCode()",
         "m.GetHashCode() = 41"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 100 100\n"));
}

// #6015 (the issue's choice repro) / §35.7, §14.2 (v0.338): P(5) vs P(15) are
// equal by the declared member and different by field; the body reads the
// argument's arm, which pre-fix was the box header (MatchFailureException).
BEZEL_TEST(interface_object_slot_witness_14_2_6015,
           choice_declared_equals_through_interface_reads_the_payload) {
    JitResult r = runAotChecked(countingProg(
        kChoice,
        "      var i: IEq := C.P(5);\n"
        "      var q: IEq := C.Q;\n" +
            holder("hEq", "C.P(15)") + holder("hSame", "C.P(5)") +
            holder("hNe", "C.P(6)") + holder("hArm", "C.Q"),
        {"i.Equals(hEq.O)", "i.Equals(hSame.O)", "i.Equals(hNe.O)",
         "i.Equals(hArm.O)", "q.Equals(hArm.O)", "q.Equals(hEq.O)"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 100 0 0 100 0\n"));
}

BEZEL_TEST(interface_object_slot_witness_14_2_6015,
           choice_interface_equals_null_and_foreign_argument_is_unequal) {
    JitResult r = runAotChecked(countingProg(
        kChoice,
        "      var i: IEq := C.P(5);\n" +
            holder("hNull", nullptr) + holder("hInt", "5") +
            holder("hChoice", "D.P(5)") + holder("hRec", "new Other(5)"),
        {"i.Equals(hNull.O)", "i.Equals(null)", "i.Equals(hInt.O)",
         "i.Equals(\"s\")", "i.Equals(hChoice.O)", "i.Equals(hRec.O)"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0 0 0 0 0 0\n"));
}

// The interface slot and the `Object` slot are ONE member (§14.2): the same
// two boxes answer alike whichever static type the call goes through, and the
// hash agrees with the equality on both.
BEZEL_TEST(interface_object_slot_witness_14_2_6015,
           choice_interface_and_object_slot_agree) {
    JitResult r = runAotChecked(countingProg(
        kChoice,
        "      var a: C := C.P(5);\n"
        "      var i: IEq := a;\n"
        "      var o: Object := a;\n"
        "      var k: IEq := C.P(15);\n" +
            holder("hEq", "C.P(15)"),
        {"i.Equals(hEq.O)", "o.Equals(hEq.O)", "o.Equals(i)", "o.Equals(k)",
         "i.GetHashCode() = o.GetHashCode()",
         "i.GetHashCode() = k.GetHashCode()", "i.GetHashCode() = 505"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("100 100 100 100 100 100 100\n"));
}

// §9.2 (v0.338): the witness is the kind's member for the slot, "declared or
// generated". An enum declaring nothing: pre-fix the table entry was null and
// the call was a jump to address 0.
BEZEL_TEST(interface_object_slot_witness_14_2_6015,
           enum_generated_members_witness_the_interface) {
    JitResult r = runAotChecked(countingProg(
        kPlain,
        "      var i: IEq := E.B;\n"
        "      var k: IEq := E.B;\n"
        "      var m: IEq := E.C;\n" +
            holder("hSame", "E.B") + holder("hNe", "E.C") +
            holder("hNull", nullptr) + holder("hInt", "1"),
        {"i.Equals(hSame.O)", "i.Equals(hNe.O)", "i.Equals(hNull.O)",
         "i.Equals(hInt.O)", "i.GetHashCode() = k.GetHashCode()",
         "i.GetHashCode() = m.GetHashCode()"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 0 0 0 100 0\n"));
}

// The same for a choice declaring nothing: member-wise equality on the arm and
// its fields (§35.7), reached through the interface.
BEZEL_TEST(interface_object_slot_witness_14_2_6015,
           choice_generated_members_witness_the_interface) {
    JitResult r = runAotChecked(countingProg(
        kPlain,
        "      var i: IEq := C.P(5);\n"
        "      var k: IEq := C.P(5);\n"
        "      var m: IEq := C.P(15);\n" +
            holder("hSame", "C.P(5)") + holder("hNe", "C.P(15)") +
            holder("hArm", "C.Q") + holder("hNull", nullptr),
        {"i.Equals(hSame.O)", "i.Equals(hNe.O)", "i.Equals(hArm.O)",
         "i.Equals(hNull.O)", "i.GetHashCode() = k.GetHashCode()",
         "i.GetHashCode() = m.GetHashCode()"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 0 0 0 100 0\n"));
}

// §14.2 (v0.338): `ToString(): string` in an interface is witnessed by the
// generated rendering (§10.5 / §35.7). The rendered text is asserted, and the
// loop makes a dropped or doubled release of the fresh string visible.
BEZEL_TEST(interface_object_slot_witness_14_2_6015,
           generated_tostring_witnesses_the_interface) {
    JitResult r = runAotChecked(
        std::string(kPrelude) + kPlain +
        "public class Program\n"
        "  public static method Main()\n"
        "    var n := 0;\n"
        "    for var j in 0..<100 do\n"
        "      var e: IShow := E.B;\n"
        "      var c: IShow := C.P(j);\n"
        "      n := n + e.ToString().Length + c.ToString().Length;\n"
        "    end for;\n"
        "    var e: IShow := E.C;\n"
        "    var c: IShow := C.P(7);\n"
        "    var q: IShow := C.Q;\n"
        "    Console.WriteLine(e.ToString());\n"
        "    Console.WriteLine(c.ToString());\n"
        "    Console.WriteLine(q.ToString());\n"
        "    Console.WriteLine(n.ToString());\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    // An enum's default rendering is its qualified name (§8.5): "M.E".
    // 100 x "M.E" (3) + 10 x "C.P(x: d)" (9) + 90 x "C.P(x: dd)" (10).
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("M.E\nC.P(x: 7)\nC.Q\n1290\n"));
}
