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

// ---------------------------------------------------------------------------
// fix #6015 (fix-forward of the #5924 item 3 thunks): a pair declared OFF the
// `Object` slot's exact shape keeps the box slots it had before the declared
// thunks existed.
//
// The thunks return the declared member's result as the slot's own `i1` /
// `i32`. §8.1 (v0.338) gives the slot's shape — `Equals(other: Object?): bool`,
// `GetHashCode(): int` — and §9.2 (v0.338) names the value kind's member "at
// `GetHashCode(): int`". The pair was matched by name and parameter list only,
// so a member returning `int?` / `bool?` (an optional aggregate) or any other
// type made the thunk `ret` an ill-typed value and the MODULE failed IR
// verification — with no boxing anywhere in the program (the two sema rows
// record_override_{gethashcode,equals}_nullable_return_unchanged_pending_6011
// declare the record and call the member directly). A member with a parameter
// mode, a generic member or a `uint` hash type-checked and silently ran
// through the box instead: `Equals(out other: R)` WROTE through the
// argument's box.
//
// Whether such a member fills the slot is spec-question #6011 (open), so these
// rows rule nothing: they pin the behaviour the same programs had BEFORE the
// declared thunks — member-wise equality and hash through the box, the
// generated members of §9.1 / §35.7 — and each fails without the fix (the
// first four at compile time, the rest on the counters).
//
// Counters, in order: Equals with a key-equal / member-wise-equal / different
// argument, a fourth argument (null for a record, another arm for a choice),
// a foreign argument (a boxed int / null); then GetHashCode against a
// key-equal box, a member-wise-equal box, and the declared hash's value.
// Member-wise through the box is "0 100 0 0 0 0 100 0"; the declared pair
// would answer "100 100 0 0 0 100 100 100".
//
// No row goes through `Set<Object>` / `Dictionary<Object, V>`: every box
// handed to those containers leaks 24 bytes independently of this change
// (#4717, measured on the pre-#5924 compiler too), so a checked row there
// cannot be green on any compiler.

namespace {

const char* kStructuralThroughBox = "0 100 0 0 0 0 100 0\n";

std::string recordWith(const std::string& eq, const std::string& hash) {
    return "public record R(X: int, Y: int)\n" + eq + hash + "end R;\n";
}
std::string choiceWith(const std::string& eq, const std::string& hash) {
    return "public choice C\n"
           "  P(x: int), Q;\n"
           "  public method Key(): int => (case this of P(v) => v mod 10, Q => -1);\n" +
           eq + hash + "end C;\n";
}

const char* kRecEq =
    "  public method Equals(other: R): bool => (X mod 10) = (other.X mod 10);\n";
const char* kRecHash =
    "  public method GetHashCode(): int => (X mod 10) + 500;\n";
const char* kChoEq =
    "  public method Equals(other: C): bool => Key() = other.Key();\n";
const char* kChoHash =
    "  public method GetHashCode(): int => Key() + 500;\n";

JitResult runRecordThroughBox(const std::string& decls) {
    return runAotChecked(countingProg(
        decls,
        "      var o: Object := new R(5, 1);\n"
        "      var p: Object := new R(15, 1);\n"
        "      var q: Object := new R(5, 1);\n" +
            holder("hEq", "new R(15, 1)") + holder("hSame", "new R(5, 1)") +
            holder("hNe", "new R(6, 1)") + holder("hNull", nullptr) +
            holder("hInt", "5"),
        {"o.Equals(hEq.O)", "o.Equals(hSame.O)", "o.Equals(hNe.O)",
         "o.Equals(hNull.O)", "o.Equals(hInt.O)",
         "o.GetHashCode() = p.GetHashCode()",
         "o.GetHashCode() = q.GetHashCode()", "o.GetHashCode() = 505"}));
}

JitResult runChoiceThroughBox(const std::string& decls) {
    return runAotChecked(countingProg(
        decls,
        "      var o: Object := C.P(5);\n"
        "      var p: Object := C.P(15);\n"
        "      var q: Object := C.P(5);\n" +
            holder("hEq", "C.P(15)") + holder("hSame", "C.P(5)") +
            holder("hNe", "C.P(6)") + holder("hArm", "C.Q") +
            holder("hNull", nullptr),
        {"o.Equals(hEq.O)", "o.Equals(hSame.O)", "o.Equals(hNe.O)",
         "o.Equals(hArm.O)", "o.Equals(hNull.O)",
         "o.GetHashCode() = p.GetHashCode()",
         "o.GetHashCode() = q.GetHashCode()", "o.GetHashCode() = 505"}));
}

}  // namespace

// The sema row's own record (`override GetHashCode(): int?` beside an
// own-typed `Equals`), now BOXED: `Object.Equals` / `Object.GetHashCode` on it
// compile, run, and answer member-wise as before. Pending #6011.
BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           record_nullable_gethashcode_object_receiver_unchanged_pending_6011) {
    JitResult r = runRecordThroughBox(recordWith(
        "  public override method Equals(other: R): bool =>\n"
        "    (X mod 10) = (other.X mod 10);\n",
        "  public override method GetHashCode(): int? => (X mod 10) + 500;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(kStructuralThroughBox));
}

// The other sema row's record (`override Equals(other: R): bool?`), boxed.
BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           record_nullable_equals_object_receiver_unchanged_pending_6011) {
    JitResult r = runRecordThroughBox(recordWith(
        "  public override method Equals(other: R): bool? =>\n"
        "    (X mod 10) = (other.X mod 10);\n",
        "  public override method GetHashCode(): int => (X mod 10) + 500;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(kStructuralThroughBox));
}

// The same two nullable returns on a choice. Pending #6011.
BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           choice_nullable_pair_object_receiver_unchanged_pending_6011) {
    JitResult h = runChoiceThroughBox(choiceWith(
        kChoEq, "  public method GetHashCode(): int? => Key() + 500;\n"));
    BEZEL_EXPECT_TRUE(h.ran);
    BEZEL_EXPECT_EQ(h.stdoutText, std::string(kStructuralThroughBox));
    JitResult e = runChoiceThroughBox(choiceWith(
        "  public method Equals(other: C): bool? => Key() = other.Key();\n",
        kChoHash));
    BEZEL_EXPECT_TRUE(e.ran);
    BEZEL_EXPECT_EQ(e.stdoutText, std::string(kStructuralThroughBox));
}

// A plain member at another NON-nullable return type (`GetHashCode(): long`
// on a record, `Equals(other: C): int` on a choice) is not at the slot's shape
// either (§8.1, v0.338): it failed IR verification the same way.
BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           other_return_type_object_receiver_unchanged) {
    JitResult h = runRecordThroughBox(recordWith(
        kRecEq,
        "  public method GetHashCode(): long => long((X mod 10) + 500);\n"));
    BEZEL_EXPECT_TRUE(h.ran);
    BEZEL_EXPECT_EQ(h.stdoutText, std::string(kStructuralThroughBox));
    JitResult e = runChoiceThroughBox(choiceWith(
        "  public method Equals(other: C): int => Key() + other.Key();\n",
        kChoHash));
    BEZEL_EXPECT_TRUE(e.ran);
    BEZEL_EXPECT_EQ(e.stdoutText, std::string(kStructuralThroughBox));
}

// `Equals(out other: R)` / `Equals(ref other: R)`: the thunk type-checked (the
// parameter is a pointer either way) and handed the member the ARGUMENT's box
// payload as its `out` / `ref` target, so the `out` body overwrote the boxed
// argument, and every argument of the receiver's type compared equal.
BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           equals_parameter_mode_object_receiver_unchanged) {
    JitResult o = runRecordThroughBox(recordWith(
        "  public method Equals(out other: R): bool\n"
        "    other := new R(X, Y);\n"
        "    return true;\n"
        "  end Equals;\n",
        kRecHash));
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string(kStructuralThroughBox));
    JitResult f = runRecordThroughBox(recordWith(
        "  public method Equals(ref other: R): bool =>\n"
        "    (X mod 10) = (other.X mod 10);\n",
        kRecHash));
    BEZEL_EXPECT_TRUE(f.ran);
    BEZEL_EXPECT_EQ(f.stdoutText, std::string(kStructuralThroughBox));
}

// A GENERIC member (`Equals<U>(other: R)`, `GetHashCode<U>()`) and a `uint`
// hash also type-checked and ran through the box.
BEZEL_TEST(box_object_slot_declared_equals_9_2_5924,
           generic_member_and_uint_hash_object_receiver_unchanged) {
    JitResult g = runRecordThroughBox(recordWith(
        "  public method Equals<U>(other: R): bool =>\n"
        "    (X mod 10) = (other.X mod 10);\n",
        kRecHash));
    BEZEL_EXPECT_TRUE(g.ran);
    BEZEL_EXPECT_EQ(g.stdoutText, std::string(kStructuralThroughBox));
    JitResult c = runChoiceThroughBox(choiceWith(
        kChoEq, "  public method GetHashCode<U>(): int => Key() + 500;\n"));
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.stdoutText, std::string(kStructuralThroughBox));
    JitResult u = runRecordThroughBox(recordWith(
        kRecEq,
        "  public method GetHashCode(): uint => uint((X mod 10) + 500);\n"));
    BEZEL_EXPECT_TRUE(u.ran);
    BEZEL_EXPECT_EQ(u.stdoutText, std::string(kStructuralThroughBox));
}
