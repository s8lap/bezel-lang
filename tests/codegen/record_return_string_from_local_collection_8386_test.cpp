// #8386, the RETURN face — a record built from a string read out of a LOCAL
// collection and returned by value handed the caller a dangling component:
// the collection is disposed at that same return, and it held the only
// reference on the string.
//
// THE RULE. Spec §9.7 (v0.357): "Reference-typed fields whose declared types
// are runtime-managed (`string`, `Type`, function types) do not make the
// record borrow-bearing because runtime-managed values have unbounded lifetime
// by construction". §28.3.1 K1.6 (v0.357): "Values of runtime-managed type
// (`string`, `Type`, boxed value types, function values) and of ordinary value
// type carry no classification and no obligation". §28.9 (v0.357): "the type's
// lifetime is managed by the runtime without user involvement." So the `Name`
// of a record a method returns is the string, whatever became of the
// collection it was read from.
//
// THE DEFECT. `1cae10ffd` (narrow #8386) cured `rows.Add(new E(f[0]))`, where
// the `Array<E>` became the holder of the string. The same construction
// RETURNED had no holder at all:
//     var f := line.Split(',');
//     return new E(f[0], n);              // or: var e := new E(f[0], n); return e;
// A by-value return is a bit copy; the return's disposals free `f`, and with
// it `f[0]`. MEASURED on r30-c1 e17797b8d: every row below prints `0` instead
// of `100` (the freed block is reused by the next allocation, so `Name` reads
// another string's text); under ASan, heap-use-after-free.
//
// THE FIX. The record's positional store retains a string read by indexer
// from a local collection the function disposes — at a `return new E(...)`
// directly (`collectReturnedRecordStringArgsToRetain`), and for `var e := new
// E(...)` through a hidden holder released at the local's scope end, from
// which a later `return e;` takes the returned copy's reference
// (`lowerLocalDeclHoldingRecordStrings`, `retainReturnedRecordLocalStrings`;
// LlvmBackend_Exprs2.cpp).
//
// ORACLE. `runAot` — the real AOT path; under `BEZEL_AOT_FORCE_SANITIZE=address`
// the use-after-free is a sanitizer finding. NOT `runAotChecked` for the fix
// rows: no caller releases a by-value result's strings (#8267 tracks that
// caller-side release; the `+1`-producer twin `return new E(f[0] + "x", n)`
// leaks the same way before and after), so the fix leaves one retained string
// per returning call, which strict LeakSanitizer would report. Every fix row
// loops 100 times, churns the allocator after each call (so a dangling `Name`
// cannot keep its old text by luck) and compares every component by CONTENT.
// The two controls at the end DO run under `runAotChecked`: they pin the two
// places where the fix must not leave a reference behind.

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `decls`: the record declarations. `ret` / `body`: the return type and body
// of `Program.Get(i)`. `ok`: the condition, over `e` (the result) and `i`,
// that holds exactly when every component of the result has its own text.
// `extra`: further members of `Program`.
std::string program(const std::string& decls, const std::string& ret,
                    const std::string& body, const std::string& ok,
                    const std::string& extra = "") {
    return
        "module P;\n" + decls + "\n"
        "public class Program\n" + extra +
        "  static method Get(i: int): " + ret + "\n" + body + "\n"
        "  end Get;\n"
        "  public static method Main(): int\n"
        "    var ok := 0;\n"
        "    for var i in 1..100 do\n"
        "      var e := Program.Get(i);\n"
        "      var junk := $\"JUNK{i},yy{i}\".Split(',');\n"
        "      if " + ok + " then ok := ok + 1; end;\n"
        "    end;\n"
        "    Console.WriteLine(ok);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
}

const char* kE = "record E(Name: string, N: int);";
const char* kSplit = "    var f := $\"name{i},zz{i}\".Split(',');\n";
const char* kNameOk = "e.Name = $\"name{i}\" and e.N = i";

void expectAllHundred(const std::string& prog) {
    JitResult r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}

// The harness SKIPS (loudly, counted) when clang is absent; a sanitizer finding
// is NOT a skip — "sanitizer fired" means it ran and found something.
bool skipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// Crash oracle AND leak oracle: the program runs to completion under ASan,
// prints `100`, and reports neither a leak nor an over-release.
void expectAllHundredAndNoLeak(const std::string& prog) {
    JitResult r = runAotChecked(prog);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("over-release") == std::string::npos);
        BEZEL_EXPECT_TRUE(d.message.find("byte(s) leaked") == std::string::npos);
        BEZEL_EXPECT_TRUE(d.message.find("use-after-free") == std::string::npos);
    }
}

}  // namespace

// The issue's construction on the return road: `f` dies at the return.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_new_record_of_split_element_spec_9_7) {
    expectAllHundred(program(kE, "E",
        std::string(kSplit) + "    return new E(f[0], i);", kNameOk));
}

// A nested construction is the returned value too.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_nested_record_of_split_element_spec_9_7) {
    expectAllHundred(program(
        std::string(kE) + "\nrecord Outer(In: E, K: int);", "Outer",
        std::string(kSplit) + "    return new Outer(new E(f[0], i), 2);",
        "e.In.Name = $\"name{i}\" and e.In.N = i and e.K = 2"));
}

// A generic record instantiated at `string`.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_generic_record_of_split_element_spec_9_7) {
    expectAllHundred(program("record Box<T>(V: T, N: int);", "Box<string>",
        std::string(kSplit) + "    return new Box<string>(f[0], i);",
        "e.V = $\"name{i}\" and e.N = i"));
}

// Named arguments reach the same positional store.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_new_record_named_arguments_spec_9_7) {
    expectAllHundred(program(kE, "E",
        std::string(kSplit) + "    return new E(N := i, Name := f[0]);",
        kNameOk));
}

// A `string?` component fed by a `string` element.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_record_with_nullable_string_component_spec_28_9) {
    expectAllHundred(program("record EN(Name: string?, N: int);", "EN",
        std::string(kSplit) + "    return new EN(f[0], i);",
        "(e.Name ?? \"\") = $\"name{i}\" and e.N = i"));
}

// The same element in two components: both stay the string.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_record_same_element_in_two_components_spec_28_9) {
    expectAllHundred(program("record E3(A: string, B: string, N: int);", "E3",
        std::string(kSplit) + "    return new E3(f[0], f[1], i);",
        "e.A = $\"name{i}\" and e.B = $\"zz{i}\" and e.N = i"));
}

// A `Dictionary<int, string>` local is a collection the return disposes too.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_new_record_of_dictionary_value_spec_28_9) {
    expectAllHundred(program(kE, "E",
        "    var d := new Dictionary<int, string>();\n"
        "    d.Add(1, $\"name{i}\");\n"
        "    return new E(d[1], i);", kNameOk));
}

// Either arm of a conditional expression is the returned value.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_conditional_of_two_constructions_spec_9_7) {
    expectAllHundred(program(kE, "E",
        std::string(kSplit) +
        "    return (if i mod 2 = 0 then new E(f[0], i) else new E(f[1], i));",
        "e.Name = (if i mod 2 = 0 then $\"name{i}\" else $\"zz{i}\") and e.N = i"));
}

// A nullable record return wraps the same value.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_new_record_as_nullable_record_spec_9_7) {
    expectAllHundred(program(kE, "E?",
        std::string(kSplit) +
        "    if i = 0 then return null; end;\n"
        "    return new E(f[0], i);",
        "e <> null and e!.Name = $\"name{i}\" and e!.N = i"));
}

// The return leaves through a `finally`.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_new_record_through_finally_spec_28_9) {
    expectAllHundred(program(kE, "E",
        std::string(kSplit) +
        "    try\n"
        "      return new E(f[0], i);\n"
        "    finally\n"
        "      Program.Tick();\n"
        "    end try;",
        kNameOk, "  static method Tick() end Tick;\n"));
}

// The caller STORES the result: before the fix the co-owning `Array<E>`
// retained freed memory at `Add`.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           returned_record_stored_into_array_by_caller_spec_28_11) {
    expectAllHundred(program(kE, "E",
        std::string(kSplit) + "    return new E(f[0], i);",
        "Program.Keep(e, i)",
        "  static method Keep(e: E, i: int): bool\n"
        "    var rows := new Array<E>();\n"
        "    rows.Add(e); rows.Add(e);\n"
        "    rows.RemoveAt(0);\n"
        "    var junk := $\"KEEP{i},kk{i}\".Split(',');\n"
        "    return rows[0].Name = $\"name{i}\" and rows[0].N = i;\n"
        "  end Keep;\n"));
}

// The two-statement spelling.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_record_local_built_from_split_element_spec_9_7) {
    expectAllHundred(program(kE, "E",
        std::string(kSplit) +
        "    var e := new E(f[0], i);\n"
        "    return e;", kNameOk));
}

// ... through a runtime `const`.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_record_const_built_from_split_element_spec_9_7) {
    expectAllHundred(program(kE, "E",
        std::string(kSplit) +
        "    const e := new E(f[0], i);\n"
        "    return e;", kNameOk));
}

// ... returned from inside the loop that declares it (a "find" loop).
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_record_local_from_inside_its_loop_spec_9_7) {
    expectAllHundred(program(kE, "E",
        "    for var k in 1..5 do\n"
        "      var f := $\"name{i},{k}\".Split(',');\n"
        "      var e := new E(f[0], i);\n"
        "      if k = 4 then return e; end;\n"
        "    end;\n"
        "    return new E(\"none\", 0);", kNameOk));
}

// ... with the collection CLEARED between the declaration and the return: the
// local's holder, not the collection, keeps the string.
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_record_local_after_source_cleared_spec_28_9) {
    expectAllHundred(program(kE, "E",
        std::string(kSplit) +
        "    var e := new E(f[0], i);\n"
        "    f.Clear();\n"
        "    return e;", kNameOk));
}

// ... reassigned on the even iterations: the odd ones return the record that
// carries the held string, the even ones a literal one (nothing to take).
BEZEL_TEST(record_return_string_from_local_collection_8386,
           return_record_local_reassigned_on_some_paths_spec_9_7) {
    expectAllHundred(program(kE, "E",
        std::string(kSplit) +
        "    var e := new E(f[0], i);\n"
        "    if i mod 2 = 0 then e := new E(\"default\", 0); end;\n"
        "    return e;",
        "(if i mod 2 = 0 then e.Name = \"default\" and e.N = 0 "
        "else e.Name = $\"name{i}\" and e.N = i)"));
}

// CONTROL 1 (green before and after; strict leak oracle). A record local that
// is NOT returned on the path taken — a transient, a `continue`, a "find" loop
// that never matches — must give back what its holder took. Goes RED under the
// wrong fix "retain at the declaration, release nowhere" (one string leaked
// per iteration).
BEZEL_TEST(record_return_string_from_local_collection_8386,
           control_record_local_not_returned_gives_back_its_holder_spec_28_9) {
    expectAllHundredAndNoLeak(program(kE, "int",
        "    var t := 0;\n"
        "    for var k in 1..4 do\n"
        "      var f := $\"name{i},{k}\".Split(',');\n"
        "      var e := new E(f[0], k);\n"
        "      if k = 99 then return e.N; end;\n"
        "      if k = 2 then continue; end;\n"
        "      if e.Name = $\"name{i}\" then t := t + e.N; end;\n"
        "    end;\n"
        "    return t;", "e = 8"));
}

// CONTROL 2 (green before and after; strict leak oracle). A source that
// OUTLIVES the call — a parameter, a local alias of it — keeps the pre-fix
// borrow, in both spellings. Goes RED under the wrong fix "retain every
// string read at a return" (one reference per call that nobody releases).
BEZEL_TEST(record_return_string_from_local_collection_8386,
           control_record_of_parameter_element_takes_no_reference_spec_28_9) {
    expectAllHundredAndNoLeak(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "public class Program\n"
        "  static method Direct(src: Array<string>, i: int): E\n"
        "    return new E(src[0], i);\n"
        "  end Direct;\n"
        "  static method TwoStep(src: Array<string>, i: int): E\n"
        "    var e := new E(src[0], i);\n"
        "    return e;\n"
        "  end TwoStep;\n"
        "  static method Alias(src: Array<string>, i: int): E\n"
        "    var g := src;\n"
        "    return new E(g[0], i);\n"
        "  end Alias;\n"
        "  public static method Main(): int\n"
        "    var ok := 0;\n"
        "    for var i in 1..100 do\n"
        "      var src := $\"name{i},zz{i}\".Split(',');\n"
        "      var a := Program.Direct(src, i);\n"
        "      var b := Program.TwoStep(src, i);\n"
        "      var c := Program.Alias(src, i);\n"
        "      if a.Name = $\"name{i}\" and b.Name = $\"name{i}\" and\n"
        "         c.Name = $\"name{i}\" and a.N + b.N + c.N = 3 * i then\n"
        "        ok := ok + 1;\n"
        "      end;\n"
        "    end;\n"
        "    Console.WriteLine(ok);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
}
