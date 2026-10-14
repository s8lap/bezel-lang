// Regression tests for #5924 item 3 — a BOXED record or choice that declares
// its own-typed `Equals(other: T)` with `GetHashCode` answered `Object.Equals`
// and `Object.GetHashCode` by its FIELDS: the box's `Object`-slot thunks were
// the structural ones and consulted no user member. A direct call, a hash
// container and the unboxed value all answered by the declaration, so one
// value had two equalities depending on whether it had been boxed.
//
// Spec: §9.2 (v0.338): on every value kind the own-typed `Equals` is "the one
// member that fills the kind's `Object` slot ... Through the slot ... the
// member runs only on a `T` payload and any other argument compares unequal
// without the body running." §35.7 (v0.338): "Where the choice declares the
// member, the box reaches it"; §8.3 (v0.338) pairs it with `GetHashCode`.
//
// Every declared `Equals` here READS its argument (a key reduced mod 10), so
// two structurally DIFFERENT values compare equal and a thunk that misreads
// the payload cannot answer by accident. Rows loop 100x on the checked oracle
// and assert stdout: counters of how many iterations answered `true`.
//
// The argument is read back out of a holder's `Object?` property and the
// receiver is a local: those shapes release every box they make, so a leak
// report under the sanitizer is this fix's and not the surrounding program's.
//
// Generic records and choices are NOT covered: their box slots are filled per
// instantiation and are a separate follow-up.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

const char* kHolder =
    "module M;\n"
    "public class H\n"
    "  public property O: Object? := null;\n"
    "end H;\n"
    "public record Other(Q: int);\n";

// A choice whose declared equality is its payload mod 10; `Q` is its own key.
const char* kChoice =
    "public choice C\n"
    "  P(x: int), Q;\n"
    "  public method Key(): int => (case this of P(v) => v mod 10, Q => -1);\n"
    "  public method Equals(other: C): bool => Key() = other.Key();\n"
    "  public method GetHashCode(): int => Key() + 500;\n"
    "end C;\n";

// A positional record whose declared equality reads X mod 10 and ignores Y.
const char* kRecord =
    "public record R(X: int, Y: int)\n"
    "  public method Equals(other: R): bool => (X mod 10) = (other.X mod 10);\n"
    "  public method GetHashCode(): int => (X mod 10) + 500;\n"
    "end R;\n";

// A record with a string field beside the key: the declared member takes the
// payload by address and reads the string through it.
const char* kStringRecord =
    "public record S(Name: string, X: int)\n"
    "  public method Equals(other: S): bool =>\n"
    "    (X mod 10) = (other.X mod 10) and Name.Length = other.Name.Length;\n"
    "  public method GetHashCode(): int => (X mod 10) + Name.Length;\n"
    "end S;\n";

// `checks` is a list of bool expressions; the program counts, over 100
// iterations, how often each is true and prints the counters space-separated.
std::string countingProg(const std::string& decls, const std::string& setup,
                         const std::vector<std::string>& checks) {
    std::string s = std::string(kHolder) + decls +
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

// One holder per argument, assigned once.
std::string holder(const char* name, const char* value) {
    return std::string("      using var ") + name + " := new H();\n" +
           (value ? std::string("      ") + name + ".O := " + value + ";\n"
                  : std::string());
}

}  // namespace

// #5924 item 3 / §35.7 (v0.338): `Object.Equals` on a boxed choice reaches the
// declared member. P(5) vs P(15): equal by declaration, different by field.
BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           choice_object_equals_runs_declared_member) {
    JitResult r = runAotChecked(countingProg(
        kChoice,
        "      var o: Object := C.P(5);\n" +
            holder("hEq", "C.P(15)") + holder("hSame", "C.P(5)") +
            holder("hNe", "C.P(6)") + holder("hArm", "C.Q"),
        {"o.Equals(hEq.O)", "o.Equals(hSame.O)", "o.Equals(hNe.O)",
         "o.Equals(hArm.O)"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 100 0 0\n"));
}

// §9.2 (v0.338): through the slot "any other argument compares unequal without
// the body running" — null, a boxed int, a string, a boxed value of another
// type. The body would match on a garbage tag if it ran on any of them.
BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           choice_object_equals_foreign_argument_is_unequal) {
    JitResult r = runAotChecked(countingProg(
        kChoice,
        "      var o: Object := C.P(5);\n" +
            holder("hNull", nullptr) + holder("hInt", "5") +
            holder("hRec", "new Other(5)"),
        {"o.Equals(hNull.O)", "o.Equals(hInt.O)", "o.Equals(\"s\")",
         "o.Equals(hRec.O)"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0 0 0 0\n"));
}

// §8.3 / §35.7 (v0.338): the boxed `GetHashCode` is the declared one, so it
// agrees with the unboxed value's and with an equal value's.
BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           choice_object_hashcode_runs_declared_member) {
    JitResult r = runAotChecked(countingProg(
        kChoice,
        "      var a: C := C.P(5);\n"
        "      var o: Object := a;\n"
        "      var p: Object := C.P(15);\n"
        "      var q: Object := C.Q;\n",
        {"o.GetHashCode() = 505", "o.GetHashCode() = a.GetHashCode()",
         "o.GetHashCode() = p.GetHashCode()", "q.GetHashCode() = 499"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 100 100 100\n"));
}

// #5924 item 3 / §9.2 (v0.338): the same on a record. R(5, 1) vs R(15, 1) and
// R(5, 2) are equal by declaration and different by field.
BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           record_object_equals_runs_declared_member) {
    JitResult r = runAotChecked(countingProg(
        kRecord,
        "      var o: Object := new R(5, 1);\n" +
            holder("hEq", "new R(15, 1)") + holder("hY", "new R(5, 2)") +
            holder("hNe", "new R(6, 1)") + holder("hNull", nullptr) +
            holder("hInt", "5") + holder("hRec", "new Other(5)"),
        {"o.Equals(hEq.O)", "o.Equals(hY.O)", "o.Equals(hNe.O)",
         "o.Equals(hNull.O)", "o.Equals(hInt.O)", "o.Equals(\"s\")",
         "o.Equals(hRec.O)"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 100 0 0 0 0 0\n"));
}

BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           record_object_hashcode_runs_declared_member) {
    JitResult r = runAotChecked(countingProg(
        kRecord,
        "      var a: R := new R(5, 1);\n"
        "      var o: Object := a;\n"
        "      var p: Object := new R(15, 7);\n",
        {"o.GetHashCode() = 505", "o.GetHashCode() = a.GetHashCode()",
         "o.GetHashCode() = p.GetHashCode()"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 100 100\n"));
}

// A record with a string field: the declared member reads both payloads'
// strings through the boxes (their lengths decide equality beside the key).
// The strings are literals: a box of a record owning a FRESH string leaks it
// before this fix and independently of it, which would drown the row.
BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           record_with_string_field_object_equals_runs_declared_member) {
    JitResult r = runAotChecked(countingProg(
        kStringRecord,
        "      var o: Object := new S(\"ab\", 5);\n" +
            holder("hEq", "new S(\"cd\", 15)") +
            holder("hLen", "new S(\"abcdef\", 5)") +
            holder("hNe", "new S(\"ab\", 6)"),
        {"o.Equals(hEq.O)", "o.Equals(hLen.O)", "o.Equals(hNe.O)",
         "o.Equals(o)", "o.GetHashCode() = 7"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 0 0 100 100\n"));
}

// Control (unchanged behaviour): a record and a choice that declare NOTHING
// keep the generated member-wise equality through the box (§9.2 / §35.7).
BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           undeclared_record_and_choice_keep_structural_object_equals) {
    JitResult r = runAotChecked(countingProg(
        "public record R(X: int, Y: int);\n"
        "public choice C\n  P(x: int), Q;\nend C;\n",
        "      var o: Object := new R(5, 1);\n"
        "      var c: Object := C.P(5);\n" +
            holder("hSame", "new R(5, 1)") + holder("hNe", "new R(15, 1)") +
            holder("cSame", "C.P(5)") + holder("cNe", "C.P(15)") +
            holder("cArm", "C.Q"),
        {"o.Equals(hSame.O)", "o.Equals(hNe.O)", "c.Equals(cSame.O)",
         "c.Equals(cNe.O)", "c.Equals(cArm.O)", "o.Equals(cSame.O)"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100 0 100 0 0 0\n"));
}
