// Regression tests for #6219 (§3.6, §9.2, §25.2, v0.338): a BODY record that
// declares its own equality (own-typed `Equals`, `GetHashCode`, and the
// homogeneous `=`/`<>` pair) is hashable on the strength of those members, so it
// is a legal `Dictionary<K, V>` / `Set<T>` key. Before the fix the body spelling
// fell to the string-key fast path and failed IR verification; the positional
// spelling of the same four members worked.
//
// The members compare `X mod 10`, so 5 / 15 / 25 are one key and 6 is another:
// a row that read the key bytes structurally (or by identity) would see them
// apart. Every row asserts stdout, and loops 100x under the checked oracle.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

const char* kMembers =
    "  public method Equals(other: B): bool => (X mod 10) = (other.X mod 10);\n"
    "  public method GetHashCode(): int => (X mod 10) + 500;\n"
    "  public static method operator =(a: B, b: B): bool =>\n"
    "    (a.X mod 10) = (b.X mod 10);\n"
    "  public static method operator <>(a: B, b: B): bool =>\n"
    "    (a.X mod 10) <> (b.X mod 10);\n";

std::string decl(bool body) {
    if (body)
        return std::string("module P;\n"
                           "public record B\n"
                           "  public readonly property X: int;\n"
                           "  public constructor(x: int)\n"
                           "    X := x;\n"
                           "  end constructor;\n") + kMembers + "end B;\n";
    return std::string("module P;\npublic record B(X: int)\n") + kMembers +
           "end B;\n";
}

std::string prog(bool body, const std::string& stmts) {
    return decl(body) +
           "public class Program\n"
           "  public static method Main()\n"
           "    var n := 0;\n"
           "    for var j in 0..<100 do\n"
           "      var a: B := new B(5);\n"
           "      var b: B := new B(15);\n"
           "      var c: B := new B(6);\n" + stmts +
           "    end for;\n"
           "    Console.WriteLine(n);\n"
           "  end Main;\n"
           "end Program;\n";
}

// issue repro: Set + Dictionary membership of an equal-but-distinct key.
const char* kRepro =
    "      using var s := new Set<B>();\n"
    "      s.Add(a);\n"
    "      using var m := new Dictionary<B, int>();\n"
    "      m[a] := 1;\n"
    "      n := n + (if s.Contains(b) and m.ContainsKey(b) then 1 else 0);\n";

// ContainsKey / indexer set+get (overwrite through an equal key) / TryGetValue
// / Remove (through an equal key) / Count, 7 points per iteration.
const char* kOps =
    "      using var m := new Dictionary<B, int>();\n"
    "      m[a] := 1;\n"
    "      m[b] := 2;\n"                                  // overwrites a's slot
    "      m[c] := 7;\n"
    "      if m.Count = 2 then n := n + 1; end;\n"
    "      if m[new B(25)] = 2 then n := n + 1; end;\n"
    "      if m.ContainsKey(new B(35)) and not m.ContainsKey(new B(7)) then\n"
    "        n := n + 1;\n"
    "      end;\n"
    "      var got: int? := null;\n"
    "      if m.TryGetValue(c, out got) and got = 7 then n := n + 1; end;\n"
    "      var miss: int? := null;\n"
    "      if not m.TryGetValue(new B(8), out miss) then n := n + 1; end;\n"
    "      if m.Remove(new B(45)) then n := n + 1; end;\n"
    "      if m.Count = 1 and not m.ContainsKey(a) then n := n + 1; end;\n";

const char* kSetOps =
    "      using var s := new Set<B>();\n"
    "      s.Add(a);\n"
    "      s.Add(b);\n"
    "      s.Add(c);\n"
    "      if s.Count = 2 then n := n + 1; end;\n"
    "      if s.Contains(new B(25)) and not s.Contains(new B(7)) then\n"
    "        n := n + 1;\n"
    "      end;\n"
    "      if s.Remove(new B(35)) and s.Count = 1 then n := n + 1; end;\n";

// `in`, iteration over keys / elements, and a key read through a field.
const char* kInIter =
    "      using var m := new Dictionary<B, int>();\n"
    "      m[a] := 3;\n"
    "      m[new B(16)] := 4;\n"
    "      if b in m then n := n + 1; end;\n"
    "      var t := 0;\n"
    "      for var kv in m do t := t + kv.Key.X + kv.Value; end for;\n"
    "      if t = 28 then n := n + 1; end;\n"                // 5+3 + 16+4
    "      using var s := new Set<B>();\n"
    "      s.Add(a); s.Add(new B(16)); s.Add(new B(25));\n"
    "      if new B(35) in s then n := n + 1; end;\n"
    "      var u := 0;\n"
    "      for var e in s do u := u + e.X; end for;\n"
    "      if u = 21 then n := n + 1; end;\n";               // {5, 16}

// LINQ over the same elements: `ToSet` builds the dict-backed Set, `Distinct`
// the membership dict; both dedupe by the declared pair (5 = 15, 6 apart).
const char* kLinq =
    "      var arr := new Array<B>();\n"
    "      arr.Add(a); arr.Add(b); arr.Add(c);\n"
    "      using var s := arr.ToSet();\n"
    "      if s.Count = 2 then n := n + 1; end;\n"
    "      if arr.Distinct().Count() = 2 then n := n + 1; end;\n";

}  // namespace

// The issue's repro verbatim in shape (body record).
BEZEL_TEST(dict_body_record_user_equality_key_3_6_6219, body_record_issue_repro) {
    JitResult r = runAotChecked(prog(true, kRepro));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

BEZEL_TEST(dict_body_record_user_equality_key_3_6_6219, positional_record_issue_repro) {
    JitResult r = runAotChecked(prog(false, kRepro));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

BEZEL_TEST(dict_body_record_user_equality_key_3_6_6219, body_record_dictionary_ops) {
    JitResult r = runAotChecked(prog(true, kOps));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("700\n"));
}

BEZEL_TEST(dict_body_record_user_equality_key_3_6_6219, positional_record_dictionary_ops) {
    JitResult r = runAotChecked(prog(false, kOps));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("700\n"));
}

BEZEL_TEST(dict_body_record_user_equality_key_3_6_6219, body_record_set_ops) {
    JitResult r = runAotChecked(prog(true, kSetOps));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

BEZEL_TEST(dict_body_record_user_equality_key_3_6_6219, positional_record_set_ops) {
    JitResult r = runAotChecked(prog(false, kSetOps));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("300\n"));
}

BEZEL_TEST(dict_body_record_user_equality_key_3_6_6219, body_record_in_and_iteration) {
    JitResult r = runAotChecked(prog(true, kInIter));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("400\n"));
}

BEZEL_TEST(dict_body_record_user_equality_key_3_6_6219, positional_record_in_and_iteration) {
    JitResult r = runAotChecked(prog(false, kInIter));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("400\n"));
}

BEZEL_TEST(dict_body_record_user_equality_key_3_6_6219, body_record_linq_toset_distinct) {
    JitResult r = runAotChecked(prog(true, kLinq));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

// The positional spelling deduped STRUCTURALLY here before (3, 3): the hash half
// already called the declared pair but the equality half did not (§8.3).
BEZEL_TEST(dict_body_record_user_equality_key_3_6_6219, positional_record_linq_toset_distinct) {
    JitResult r = runAotChecked(prog(false, kLinq));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}
