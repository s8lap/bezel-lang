// fix #7140 — a built-in `string` member call at an arity no stdlib §9.6
// overload declares is ACCEPTED, and the program it produces either dies with
// SIGSEGV or prints a plausible wrong value.
//
// THE DEFECT. A built-in `string` member is a compiler intrinsic: it has no
// MethodSymbol, so §22.5's argument-count check (`checkCallArity`) never ran
// for it, and `resolveMemberAccess` typed the member by NAME alone — the
// argument count was never inspected (the CG27f arms key on `ma.member` and
// nothing else). The lowerings are the arity-discriminating half:
// `Emitter::tryLowerStringMethod` (LlvmBackend_String.cpp) keys on the member
// name AND the argument count, and the static-factory arms
// (LlvmBackend_Setup.cpp) key on the argument count alone.
//
// ⇒ ⇒ A call at a wrong arity matched NO lowering: codegen found no dispatch,
// the #2477 permissive-acceptance path degraded the slot to a zero-initialized
// value, and the program's first read of it faulted — MEASURED pre-fix:
// SIGSEGV, rc 139, fault address 0x8 (the string header's length field read
// off a null pointer).
//
// MEASURED pre-fix, the whole §9.6-declared instance surface at a wrong
// arity (member → declared arities → observed behaviour):
//
//   Substring      1 or 2  → 0: SIGSEGV;  3 and 4: the surplus arguments are
//                           DROPPED and "bc" is printed (a silent wrong value)
//   Trim           0 or 1  → 2, 3: SIGSEGV
//   TrimStart      0       → 1: SIGSEGV
//   TrimEnd        0       → 1: SIGSEGV
//   ToUpper        0       → 1, 2: SIGSEGV
//   ToLower        0       → 1: SIGSEGV
//   Replace        2       → 0, 1, 3: SIGSEGV
//   PadLeft        1 or 2  → 0: SIGSEGV;  3: the surplus argument is DROPPED
//   PadRight       1 or 2  → 0: SIGSEGV;  3: DROPPED
//   Insert         2       → 0, 1, 3: SIGSEGV
//   Remove         1 or 2  → 0: SIGSEGV;  3: DROPPED
//   Contains       1       → 0: the slot degrades to `false`;  2, 3: DROPPED
//   StartsWith     1       → 2: DROPPED
//   EndsWith       1       → 2: DROPPED
//   Equals         1 or 2  → 0: the slot degrades to `false`;  3: DROPPED
//   IndexOf        1 or 2  → 0: the slot degrades to 0;  3: DROPPED
//   LastIndexOf    1       → 0: the slot degrades to 0;  2: DROPPED
//   CompareTo      1 or 2  → 0: the slot degrades to 0;  3: DROPPED
//   GetHashCode    0       → 1: the slot degrades to 0
//   Split          1       → 0, 2: the slot degrades to an EMPTY array
//   ToCharArray    0       → 1: the slot degrades to an EMPTY array
//   AsSpan         0 or 1 or 2 → 3: the slot degrades to an EMPTY span
//
// and the STATIC surface:
//
//   string.Repeat              2 → 0, 1, 3: SIGSEGV
//   string.From                1 or 3 → 2, 4: SIGSEGV
//   string.Join                2 → 1, 3: SIGSEGV
//   string.IsNullOrWhiteSpace  1 → 0, 2: the slot degrades to `false`
//
// ⇒ ⇒ BOTH halves of the failure are ACCEPTANCE INVERSIONS, and the fix must
// REJECT: dropping the surplus argument, or coercing it, would compile a
// call the document does not define and print a plausible value for it.
//
// THE GATE. Keyed on the ARITIES stdlib §9.6 declares. Every diagnostic below
// names the expected arity AND the §9.6 declaration, so the reader never has
// to guess which shape the document defines.
//
// THE THREE MEMBERS §9.6 DOES NOT DECLARE. The INSTANCE `Repeat(count)`,
// `GetField(delim, n)` and `CountOf(sub)` are implemented by the compiler but
// are NOT declared in stdlib §9.6 — issue #7131 records that gap and asks
// whether they belong in the document and at which arity, and a document
// amendment is a coordinator decision, not this lane's. They are NOT deleted
// here (deleting a member the compiler has shipped is a behaviour flip this
// lane does not own), but they are not left crashing either: their arity is
// pinned to the arity the compiler LOWERS them at, and the diagnostic says so
// — "the compiler lowers `GetField` only at exactly 2 argument(s); stdlib §9.6
// declares no such member at all (issue #7131 …)" — so the reader is never
// told a declaration that does not exist.
//
// THE CONTROLS ARE THE POINT. This is a gate ADDED to a member, so the live
// risk is over-rejection of the arities §9.6 *does* declare. Every control
// asserts a VALUE, never "it compiled" — including `string.Join`, whose
// §9.6-declared two-argument form still joins correctly.
//
// (A SEPARATE PRE-EXISTING DEFECT, REPORTED NOT FIXED: `string.Join` declares
// `separator: string`, but the compiler ALSO admits a `char` separator — and
// that call lowers to malformed IR ("Load operand must be a pointer"), so the
// COMPILER crashes while emitting it. That is an argument-TYPE defect, not an
// arity one, and it is NOT this fix's.)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A bare `module M; class C ... end C;` shell — the string members are
// compiler-intrinsic, so no imports or hierarchy are required.
std::string prog(const std::string& mainBody) {
    return "module M;\nclass C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

// The prelude every program needs: an `Array<char>` value, for the
// `Trim(chars: IEnumerable<char>)` overload.
const char* kPrelude = "    var chars: Array<char> := ['a', 'b'];\n";

// True iff SOME error diagnostic carries `needle` — the SPECIFIC diagnostic
// the fix must produce, not merely "the program did not run".
bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// True iff the arity rejection for `member` is present, spelled the way the
// fix spells it: the arity supplied, the §9.6 declaration, and the arity that
// declaration admits.
bool arityRejected(const JitResult& r, const std::string& needle) {
    return r.diagnostics.hasErrors() && hasError(r, needle);
}

}  // namespace

// ── REJECT — the §9.6-declared INSTANCE members ───────────────────────────
// Each row is the issue's own shape: a member at an arity no §9.6 overload
// declares. Pre-fix each of these COMPILED; the ones marked below died with
// SIGSEGV at 0x8 when the degraded slot was read, and the rest printed a
// plausible wrong value.

// The issue's Repro A — `Repeat` at THREE arguments. §9.6 declares only the
// two STATIC `Repeat` forms, both at two arguments, so the three-argument
// instance call matches no §9.6 declaration and no lowering. Since fix #7706
// (stdlib §9.6, v0.350) the INSTANCE spelling is rejected as a non-member at
// EVERY arity, so the diagnostic is the non-member one, not an arity one.
// MEASURED pre-fix: compiled, and the generated program died with SIGSEGV at
// fault address 0x8.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_repeat_three_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"x\".Repeat(2, 3);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "string member `.Repeat` is not yet supported in codegen"));
}

// The issue's Repro B — `GetField` at THREE arguments. §9.6 declares no
// `GetField` at all (#7131).
// MEASURED pre-fix: compiled, and the generated program died with SIGSEGV at
// fault address 0x8.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_getfield_three_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"a,b,c\".GetField(',', 1, 2);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `GetField` on `string` supplies 3 argument(s) but the "
        "compiler lowers `GetField` only at exactly 2 argument(s); stdlib "
        "§9.6 declares no such member at all (issue #7131 asks whether the "
        "member belongs in §9.6 and at which arity)"));
}

// `CountOf` at TWO arguments — the third member stdlib §9.6 does not declare
// (#7131). MEASURED pre-fix: compiled, and printed `0` — a silent wrong value
// (the member counts two occurrences).
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_countof_two_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: int := \"aXbXc\".CountOf(\"X\", 1);\n"
                         "    Console.WriteLine(t);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `CountOf` on `string` supplies 2 argument(s) but the "
        "compiler lowers `CountOf` only at exactly 1 argument(s); stdlib §9.6 "
        "declares no such member at all (issue #7131 asks whether the member "
        "belongs in §9.6 and at which arity)"));
}

// `Substring` at THREE arguments — §9.6 declares one and two. MEASURED
// pre-fix: compiled, and printed "bc": the surplus argument was DROPPED and
// a plausible value came out.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_substring_three_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"abcdef\".Substring(1, 2, 3);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `Substring` on `string` supplies 3 argument(s) but "
        "stdlib §9.6 declares `Substring(startIndex: int): string` / "
        "`Substring(startIndex: int, length: int): string` — 1 or 2 "
        "argument(s) (§22.5; §9.6)"));
}

// `Substring` at ZERO arguments — the TOO-LOW direction the issue flagged as
// an unmeasured hypothesis. MEASURED pre-fix: compiled, and the generated
// program died with SIGSEGV at fault address 0x8.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_substring_zero_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"abcdef\".Substring();\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `Substring` on `string` supplies 0 argument(s) but "
        "stdlib §9.6 declares `Substring(startIndex: int): string` / "
        "`Substring(startIndex: int, length: int): string` — 1 or 2 "
        "argument(s) (§22.5; §9.6)"));
}

// `Trim` at TWO arguments — §9.6 declares the nullary form and the
// one-argument `IEnumerable<char>` form. MEASURED pre-fix: compiled, and the
// generated program died with SIGSEGV at fault address 0x8.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_trim_two_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"  ab  \".Trim(chars, 1);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `Trim` on `string` supplies 2 argument(s) but stdlib "
        "§9.6 declares `Trim(): string` / "
        "`Trim(chars: IEnumerable<char>): string` — 0 or 1 argument(s) "
        "(§22.5; §9.6)"));
}

// `TrimStart` at ONE argument — §9.6 declares only `TrimStart(): string`
// (the char-set overload is NOT declared). MEASURED pre-fix: compiled, and
// the generated program died with SIGSEGV at fault address 0x8.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_trimstart_one_argument) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"  ab\".TrimStart(chars);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `TrimStart` on `string` supplies 1 argument(s) but "
        "stdlib §9.6 declares `TrimStart(): string` — exactly 0 argument(s) "
        "(§22.5; §9.6)"));
}

// `TrimEnd` at ONE argument — §9.6 declares only `TrimEnd(): string`.
// MEASURED pre-fix: compiled, and the generated program died with SIGSEGV.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_trimend_one_argument) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"ab  \".TrimEnd(chars);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `TrimEnd` on `string` supplies 1 argument(s) but stdlib "
        "§9.6 declares `TrimEnd(): string` — exactly 0 argument(s) "
        "(§22.5; §9.6)"));
}

// `ToUpper` at ONE argument — §9.6 declares only `ToUpper(): string`.
// MEASURED pre-fix: compiled, and the generated program died with SIGSEGV.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_toupper_one_argument) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"ab\".ToUpper(1);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `ToUpper` on `string` supplies 1 argument(s) but stdlib "
        "§9.6 declares `ToUpper(): string` — exactly 0 argument(s) "
        "(§22.5; §9.6)"));
}

// `Replace` at THREE arguments — §9.6 declares both overloads at two.
// MEASURED pre-fix: compiled, and the generated program died with SIGSEGV.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_replace_three_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"ab\".Replace('a', 'b', 'c');\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `Replace` on `string` supplies 3 argument(s) but stdlib "
        "§9.6 declares `Replace(oldChar: char, newChar: char): string` / "
        "`Replace(oldValue: string, newValue: string): string` — exactly 2 "
        "argument(s) (§22.5; §9.6)"));
}

// `PadLeft` at THREE arguments — §9.6 declares one and two. MEASURED
// pre-fix: compiled, and the surplus argument was DROPPED ("--ab" came out).
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_padleft_three_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"ab\".PadLeft(4, '-', 1);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `PadLeft` on `string` supplies 3 argument(s) but stdlib "
        "§9.6 declares `PadLeft(totalWidth: int, paddingChar: char := ' '): "
        "string` — 1 or 2 argument(s) (§22.5; §9.6)"));
}

// `PadRight` at ZERO arguments — §9.6 declares one and two. MEASURED
// pre-fix: compiled, and the generated program died with SIGSEGV.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_padright_zero_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"ab\".PadRight();\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `PadRight` on `string` supplies 0 argument(s) but stdlib "
        "§9.6 declares `PadRight(totalWidth: int, paddingChar: char := ' '): "
        "string` — 1 or 2 argument(s) (§22.5; §9.6)"));
}

// `Insert` at ONE argument — §9.6 declares `Insert(startIndex: int, value:
// string)`. MEASURED pre-fix: compiled, and the generated program died with
// SIGSEGV.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_insert_one_argument) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"ab\".Insert(1);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `Insert` on `string` supplies 1 argument(s) but stdlib "
        "§9.6 declares `Insert(startIndex: int, value: string): string` — "
        "exactly 2 argument(s) (§22.5; §9.6)"));
}

// `Remove` at ZERO arguments — §9.6 declares one and two. MEASURED pre-fix:
// compiled, and the generated program died with SIGSEGV.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_remove_zero_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"abcdef\".Remove();\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `Remove` on `string` supplies 0 argument(s) but stdlib "
        "§9.6 declares `Remove(startIndex: int): string` / "
        "`Remove(startIndex: int, length: int): string` — 1 or 2 argument(s) "
        "(§22.5; §9.6)"));
}

// `Remove` at THREE arguments — MEASURED pre-fix: compiled, and the surplus
// argument was DROPPED ("adef" came out).
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_remove_three_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := \"abcdef\".Remove(1, 2, 3);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `Remove` on `string` supplies 3 argument(s) but stdlib "
        "§9.6 declares `Remove(startIndex: int): string` / "
        "`Remove(startIndex: int, length: int): string` — 1 or 2 argument(s) "
        "(§22.5; §9.6)"));
}

// `Contains` at TWO arguments — §9.6 declares one (the `string` and the
// `char` overloads). MEASURED pre-fix: compiled, and the surplus argument
// was DROPPED (`true` came out).
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_contains_two_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: bool := \"ab\".Contains(\"a\", \"b\");\n"
                         "    Console.WriteLine(t);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `Contains` on `string` supplies 2 argument(s) but stdlib "
        "§9.6 declares `Contains(value: string): bool` / "
        "`Contains(value: char): bool` — exactly 1 argument(s) (§22.5; §9.6)"));
}

// `Equals` at ZERO arguments — §9.6 declares one and two. MEASURED pre-fix:
// compiled, and the slot degraded to `false`.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_equals_zero_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: bool := \"ab\".Equals();\n"
                         "    Console.WriteLine(t);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `Equals` on `string` supplies 0 argument(s) but stdlib "
        "§9.6 declares `Equals(other: string): bool` / "
        "`Equals(other: string?, ignoreCase: bool): bool` — 1 or 2 "
        "argument(s) (§22.5; §9.6)"));
}

// `IndexOf` at THREE arguments — §9.6 declares one and two. MEASURED
// pre-fix: compiled, and the surplus argument was DROPPED.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_indexof_three_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: int := \"ab\".IndexOf(\"a\", 0, 5);\n"
                         "    Console.WriteLine(t);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `IndexOf` on `string` supplies 3 argument(s) but stdlib "
        "§9.6 declares `IndexOf(value: char, startIndex: int := 0): int` / "
        "`IndexOf(value: string, startIndex: int := 0): int` — 1 or 2 "
        "argument(s) (§22.5; §9.6)"));
}

// `LastIndexOf` at TWO arguments — §9.6 declares one. MEASURED pre-fix:
// compiled, and the surplus argument was DROPPED.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_lastindexof_two_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: int := \"ab\".LastIndexOf(\"a\", 0);\n"
                         "    Console.WriteLine(t);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `LastIndexOf` on `string` supplies 2 argument(s) but "
        "stdlib §9.6 declares `LastIndexOf(value: char): int` / "
        "`LastIndexOf(value: string): int` — exactly 1 argument(s) "
        "(§22.5; §9.6)"));
}

// `CompareTo` at THREE arguments — §9.6 declares one and two. MEASURED
// pre-fix: compiled, and the surplus argument was DROPPED.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_compareto_three_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: int := \"ab\".CompareTo(\"ab\", true, 1);\n"
                         "    Console.WriteLine(t);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `CompareTo` on `string` supplies 3 argument(s) but "
        "stdlib §9.6 declares `CompareTo(other: string): int` / "
        "`CompareTo(other: string, ignoreCase: bool): int` — 1 or 2 "
        "argument(s) (§22.5; §9.6)"));
}

// `GetHashCode` at ONE argument — §9.6 declares `GetHashCode(): int`.
// MEASURED pre-fix: compiled, and the slot degraded to 0.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_gethashcode_one_argument) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: int := \"ab\".GetHashCode(1);\n"
                         "    Console.WriteLine(t);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `GetHashCode` on `string` supplies 1 argument(s) but "
        "stdlib §9.6 declares `GetHashCode(): int` — exactly 0 argument(s) "
        "(§22.5; §9.6)"));
}

// `Split` at TWO arguments — §9.6 declares all three overloads at ONE
// argument. MEASURED pre-fix: compiled, and the slot degraded to an EMPTY
// array (the split into two segments never happened).
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_split_two_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t := \"a,b\".Split(',', 1);\n"
                         "    Console.WriteLine(t.Count);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `Split` on `string` supplies 2 argument(s) but stdlib "
        "§9.6 declares `Split(separator: char): owned Array<string>` / "
        "`Split(separator: string): owned Array<string>` / "
        "`Split(separators: Array<char>): owned Array<string>` — exactly 1 "
        "argument(s) (§22.5; §9.6)"));
}

// `ToCharArray` at ONE argument — §9.6 declares `ToCharArray(): owned
// Array<char>`. MEASURED pre-fix: compiled, and the slot degraded to an
// EMPTY array.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_tochararray_one_argument) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t := \"ab\".ToCharArray(1);\n"
                         "    Console.WriteLine(t.Count);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `ToCharArray` on `string` supplies 1 argument(s) but "
        "stdlib §9.6 declares `ToCharArray(): owned Array<char>` — exactly 0 "
        "argument(s) (§22.5; §9.6)"));
}

// `AsSpan` at THREE arguments — §9.6 declares zero, one and two (the
// one-argument forms are `AsSpan(start: int)` and `AsSpan(r: Range<int>)`).
// MEASURED pre-fix: compiled, and the slot degraded to an EMPTY span.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_instance_asspan_three_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t := \"ab\".AsSpan(1, 1, 1);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `AsSpan` on `string` supplies 3 argument(s) but stdlib "
        "§9.6 declares `AsSpan(): ReadOnlyStringSpan` / "
        "`AsSpan(start: int): ReadOnlyStringSpan` / "
        "`AsSpan(start: int, length: int): ReadOnlyStringSpan` / "
        "`AsSpan(r: Range<int>): ReadOnlyStringSpan` — 0, 1 or 2 "
        "argument(s) (§22.5; §9.6)"));
}

// ── REJECT — the §9.6-declared STATIC members ─────────────────────────────

// `string.Repeat` at THREE arguments — §9.6 declares both `Repeat` forms at
// two. MEASURED pre-fix: compiled, and the generated program died with
// SIGSEGV at fault address 0x8.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_static_repeat_three_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := string.Repeat('x', 3, 4);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `string.Repeat` supplies 3 argument(s) but stdlib §9.6 "
        "declares `Repeat(c: char, count: int): string` / "
        "`Repeat(unit: string, count: int): string` — exactly 2 argument(s) "
        "(§22.5; §9.6)"));
}

// `string.From` at TWO arguments — §9.6 declares the one-argument forms
// (`char` / `CString` / `WString`) and the three-argument char-array window
// form; the two-argument shape is neither. MEASURED pre-fix: compiled, and
// the generated program died with SIGSEGV at fault address 0x8.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_static_from_two_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := string.From('x', 2);\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `string.From` supplies 2 argument(s) but stdlib §9.6 "
        "declares `From(value: char): string` / "
        "`From(buffer: Array<char>, start: int, length: int): string` / the "
        "`CString` and `WString` forms (one argument each) — 1 or 3 "
        "argument(s) (§22.5; §9.6)"));
}

// `string.Join` at ONE argument — §9.6 declares `Join(separator: string,
// items: IEnumerable<string>)`. MEASURED pre-fix: compiled, and the generated
// program died with SIGSEGV at fault address 0x8.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_static_join_one_argument) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: string := string.Join(',');\n"
                         "    Console.WriteLine(t.Length);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `string.Join` supplies 1 argument(s) but stdlib §9.6 "
        "declares `Join(separator: string, items: IEnumerable<string>): "
        "string` — exactly 2 argument(s) (§22.5; §9.6)"));
}

// `string.IsNullOrWhiteSpace` at TWO arguments — §9.6 declares one.
// MEASURED pre-fix: compiled, and the slot degraded to `false`.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           reject_static_isnullorwhitespace_two_arguments) {
    auto r = runAot(prog(std::string(kPrelude) +
                         "    var t: bool := string.IsNullOrWhiteSpace(\"x\", \"y\");\n"
                         "    Console.WriteLine(t);\n"
                         "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(arityRejected(
        r,
        "the call to `string.IsNullOrWhiteSpace` supplies 2 argument(s) but "
        "stdlib §9.6 declares `IsNullOrWhiteSpace(value: string?): bool` — "
        "exactly 1 argument(s) (§22.5; §9.6)"));
}

// ── ACCEPT-CONTROLS — every §9.6-declared overload, WITH ITS VALUE ───────
// Every control asserts a VALUE. A control that stopped lowering, or that
// now prints a different string, fails here — a control that merely
// "compiled" would not.

// The string-returning INSTANCE members. The values are bracketed so the
// whitespace-significant ones (`Trim(chars)` on a set that does NOT contain
// the spaces, the `Pad*` results) are unambiguous in the expectation.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           accept_instance_string_members_with_their_values) {
    const std::string src = prog(
        "    var chars: Array<char> := ['a', 'b'];\n"
        "    Console.WriteLine(\"[\" + \"abcdef\".Substring(1) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"abcdef\".Substring(1, 2) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"  ab  \".Trim() + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"  ab  \".Trim(chars) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"  ab\".TrimStart() + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"ab  \".TrimEnd() + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"ab\".ToUpper() + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"AB\".ToLower() + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"ab\".Replace('a', 'b') + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"ab\".Replace(\"a\", \"b\") + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"ab\".PadLeft(4) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"ab\".PadLeft(4, '-') + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"ab\".PadRight(4) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"ab\".PadRight(4, '-') + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"ab\".Insert(1, \"X\") + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"abcdef\".Remove(1) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"abcdef\".Remove(1, 2) + \"]\");\n"
        "    Console.WriteLine(\"[\" + String.Repeat(\"x\", 3) + \"]\");\n"
        "    Console.WriteLine(\"[\" + \"a,b,c\".GetField(',', 1) + \"]\");\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(
        r.stdoutText,
        "[bcdef]\n[bc]\n[ab]\n[  ab  ]\n[ab]\n[ab]\n[AB]\n[ab]\n[bb]\n[bb]\n"
        "[  ab]\n[--ab]\n[ab  ]\n[ab--]\n[aXb]\n[a]\n[adef]\n[xxx]\n[b]\n");
}

// The bool- and int-returning INSTANCE members.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           accept_instance_bool_and_int_members_with_their_values) {
    const std::string src = prog(
        "    Console.WriteLine(\"ab\".Contains(\"a\"));\n"
        "    Console.WriteLine(\"ab\".StartsWith(\"a\"));\n"
        "    Console.WriteLine(\"ab\".EndsWith(\"b\"));\n"
        "    Console.WriteLine(\"ab\".Equals(\"ab\"));\n"
        "    Console.WriteLine(\"ab\".IndexOf(\"a\"));\n"
        "    Console.WriteLine(\"ab\".IndexOf(\"a\", 0));\n"
        "    Console.WriteLine(\"ab\".LastIndexOf(\"a\"));\n"
        "    Console.WriteLine(\"ab\".CompareTo(\"ab\"));\n"
        "    Console.WriteLine(\"ab\".CompareTo(\"ab\", true));\n"
        "    Console.WriteLine(\"aXbXc\".CountOf(\"X\"));\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "True\nTrue\nTrue\nTrue\n0\n0\n0\n0\n0\n2\n");
}

// The collection- and span-returning INSTANCE members.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           accept_instance_collection_members_with_their_values) {
    const std::string src = prog(
        "    Console.WriteLine(\"a,b\".Split(',').Count);\n"
        "    Console.WriteLine(\"ab\".ToCharArray().Count);\n"
        "    Console.WriteLine(\"ab\".AsSpan().Length);\n"
        "    Console.WriteLine(\"ab\".AsSpan(1).Length);\n"
        "    Console.WriteLine(\"ab\".AsSpan(1, 1).Length);\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n2\n2\n1\n1\n");
}

// The §9.6-declared STATIC members.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           accept_static_members_with_their_values) {
    const std::string src = prog(
        "    var chars: Array<char> := ['a', 'b'];\n"
        "    var parts: Array<string> := [\"ab\", \"cd\"];\n"
        "    Console.WriteLine(\"[\" + string.Repeat('x', 3) + \"]\");\n"
        "    Console.WriteLine(\"[\" + string.From('x') + \"]\");\n"
        "    Console.WriteLine(\"[\" + string.From(chars, 0, 2) + \"]\");\n"
        "    Console.WriteLine(\"[\" + string.Join(\",\", parts) + \"]\");\n"
        "    Console.WriteLine(string.IsNullOrWhiteSpace(\"x\"));\n"
        "    Console.WriteLine(string.IsNullOrWhiteSpace(\" \"));\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "[xxx]\n[x]\n[ab]\n[ab,cd]\nFalse\nTrue\n");
}

// ── ACCEPT-CONTROL — a USER-DEFINED method that shares the name ──────────
// The gate is keyed on the RECEIVER being a `string` value, so a user
// method that happens to be called `Trim` / `Remove` on a user type is
// untouched: it keeps its own arity and its own value.
BEZEL_TEST(sema_string_member_wrong_arity_7140,
           accept_user_defined_method_with_the_same_name) {
    const std::string src =
        "module M;\n"
        "public class Box\n"
        "  public method Trim(a: int, b: int, c: int): int\n"
        "    return a + b + c;\n"
        "  end Trim;\n"
        "end Box;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var b: Box := new Box();\n"
        "    Console.WriteLine(b.Trim(1, 2, 3));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "6");
}
