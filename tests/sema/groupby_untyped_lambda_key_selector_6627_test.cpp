// fix #6627 (stdlib §5 / spec §27.4 / §14.5, v0.357): an UNTYPED lambda key
// selector over record elements — `ps.GroupBy(lambda(q) => q.name)` — typed
// `g.Key` as the ELEMENT type and the group's elements as the KEY.
//
// THE DEFECT, in two parts.
//
// (1) THE SELECTOR'S PARAMETER WAS NEVER TYPED. stdlib §5 declares
//     `public method GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>>`,
//     so the key selector is a source-element lambda `(T) -> TKey` — the same
//     shape as `ToDictionary`'s key selector. §27.4 ("Parameter type
//     inference") says a parameter annotation may be omitted "when the
//     lambda's target type is known from context", and §5's signature supplies
//     that context. But `GroupBy` was ABSENT from
//     `kLinqElementLambdaOperatorNames` (src/common/BuiltinRegistry.cpp) — the
//     table that seeds an un-annotated LINQ lambda's parameter with the
//     receiver's element type. `src/common/BuiltinRegistry.hpp`'s own
//     docstring recorded the gap as an open question. So `q` stayed UNTYPED,
//     the selector's body (`q.name`) could not be typed, the selector's
//     inferred function type carried no `funcReturn`, and BOTH GroupBy typing
//     arms (`ExprTypecheck_Calls.cpp`'s lazy-receiver arm and its
//     concrete-collection arm) fell back to `TypeRef* keyTy = srcElem;`
//     ("default: identity key") — so `g.Key` was typed as the ELEMENT type.
//     MEASURED at the base (r30-c1): `"rec " + g.Key` was rejected with
//     "operator `+` is not defined for type `Person`".
//
//     The same gap is the malformed-IR variant the issue's comments record: an
//     `int` key over `string` elements (`a.GroupBy(lambda(s) => s.Length)`)
//     emitted `ret i32 %str.len.i32` against a `ptr` return — the key
//     trampoline was typed with the ELEMENT's return type.
//
// (2) THE GROUP'S ELEMENTS WERE TYPED AS THE KEY. stdlib §5 declares
//     `public interface IGrouping<TKey, T>: IEnumerable<T>` — the element is T,
////     the SECOND generic argument; the FIRST is the key. The `for var q in g`
//     path took `genericArgs[0]` for every Named receiver, so a group's
//     elements were typed as the KEY. MEASURED at the base: with a FULLY TYPED
//     selector (`lambda(q: Person) => q.name`), `for var q in g do … q.age …`
//     was rejected with "string member `.age` is not yet supported in codegen"
//     — `q` was the `string` key. This half is a defect of its own: it
//     reproduces with a typed selector, so it is not a consequence of (1).
//
// THE FIX. `GroupBy` joins `kLinqElementLambdaOperatorNames` (21 names), so
// the un-annotated selector parameter takes the receiver's element type and
// the selector's inferred return type becomes the key type; and the `for-in`
// element inference reads an `IGrouping`'s element at `genericArgs[1]`,
// mirroring codegen's `Emitter::iGroupingElemTypeRef` and the LINQ surface
// over a grouping (`typeLinqOperator`'s `isGroupingLinqRecv` arm).
//
// ORACLE. `runAot` (the strict AOT path: fork-isolated `Driver::compileFile`
// + clang link + native exec), stdout pinned byte-for-byte after normalising
// the line ending. The programs must RUN and PRINT: a wrong-typed `g.Key` is
// a compile REJECT on the base, and the malformed-IR variant is a codegen
// abort, so neither is observable in an exit code — only in what the program
// prints (or fails to print).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// stdoutText is the program's raw captured stdout; the line ending is the
// platform's. Normalising "\r\n" to "\n" makes the pinned expectation
// platform-independent without weakening it — the whole output is compared,
// so a missing or extra line still fails the row.
std::string normalisedStdout(const JitResult& r) {
    std::string out = r.stdoutText;
    std::string out2;
    out2.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
        out2.push_back(out[i]);
    }
    return out2;
}

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO. `record Person(name: string, age: int)` over
// `Array<Person>`, an un-annotated key selector reading a record field, and
// the group's key printed.
// FAILS-WITHOUT: the program is REJECTED — "operator `+` is not defined for
//   type `Person`" (the key typed as the element type).
// PASSES-WITH: "rec ann=2" / "rec bob=1".
// ===========================================================================
BEZEL_TEST(groupby_untyped_lambda_key_selector_6627,
           untyped_record_key_selector_groups_by_name) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "record Person(name: string, age: int); "
        "public class Program "
        "  public static method Main(): int "
        "    using var ps := new Array<Person>(); "
        "    ps.Add(new Person(\"ann\", 30)); "
        "    ps.Add(new Person(\"bob\", 40)); "
        "    ps.Add(new Person(\"ann\", 50)); "
        "    for var g in ps.GroupBy(lambda(q) => q.name) do "
        "      Console.WriteLine(\"rec \" + g.Key + \"=\" + g.Count().ToString()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_TRUE(normalisedStdout(r) == "rec ann=2\nrec bob=1\n");
}

// ===========================================================================
// THE TYPED-LAMBDA CONTROL — the same program with the selector's parameter
// annotated. It compiled and ran BEFORE the fix (the annotation supplied the
// parameter type the seeding table now supplies), so it is green on both arms:
// it is the admitting twin that shows the shape is genuinely admitted rather
// than rejected for an unrelated reason.
// FAILS-WITHOUT: no — this row is green on the base too.
// PASSES-WITH: "rec ann=2" / "rec bob=1".
// ===========================================================================
BEZEL_TEST(groupby_untyped_lambda_key_selector_6627,
           typed_record_key_selector_control) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "record Person(name: string, age: int); "
        "public class Program "
        "  public static method Main(): int "
        "    using var ps := new Array<Person>(); "
        "    ps.Add(new Person(\"ann\", 30)); "
        "    ps.Add(new Person(\"bob\", 40)); "
        "    ps.Add(new Person(\"ann\", 50)); "
        "    for var g in ps.GroupBy(lambda(q: Person) => q.name) do "
        "      Console.WriteLine(\"rec \" + g.Key + \"=\" + g.Count().ToString()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_TRUE(normalisedStdout(r) == "rec ann=2\nrec bob=1\n");
}

// ===========================================================================
// AN INT KEY over string elements — the issue's second comment (measured by
// lane r39-fixfwd2-7574). The selector returns `s.Length` (an `int`), so the
// key type is `int` and the group's elements are the `string` source elements.
//
// This row is where the defect reached CODEGEN: the key trampoline was typed
// with the ELEMENT's return type, so the emitted IR returned `i32` from a
// `ptr` function and `verifyModule` aborted the compile.
//
// FAILS-WITHOUT: the compile aborts — "Function return type does not match
//   operand type of return inst! ret i32 %str.len.i32" / "codegen error: IR
//   verification failed (malformed IR emitted)".
// PASSES-WITH: "len 1=1" / "len 2=1".
// ===========================================================================
BEZEL_TEST(groupby_untyped_lambda_key_selector_6627,
           untyped_string_element_int_key_groups_by_length) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Array<string>(); "
        "    a.Add(\"a\"); "
        "    a.Add(\"bb\"); "
        "    for var g in a.GroupBy(lambda(s) => s.Length) do "
        "      Console.WriteLine(\"len \" + g.Key.ToString() + \"=\" + g.Count().ToString()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_TRUE(normalisedStdout(r) == "len 1=1\nlen 2=1\n");
}

// ===========================================================================
// AN INT KEY over RECORD elements — the same shape with the key read from a
// record field (`q.age`). The selector's return type is `int`, so the key is
// `int`; the elements are still the records.
//
// FAILS-WITHOUT: the compile aborts — "ret i32 %4" against a
//   `%record.Person` return ("malformed IR emitted").
// PASSES-WITH: "age 30=2" / "age 40=1".
// ===========================================================================
BEZEL_TEST(groupby_untyped_lambda_key_selector_6627,
           untyped_record_int_key_groups_by_age) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "record Person(name: string, age: int); "
        "public class Program "
        "  public static method Main(): int "
        "    using var ps := new Array<Person>(); "
        "    ps.Add(new Person(\"ann\", 30)); "
        "    ps.Add(new Person(\"bob\", 40)); "
        "    ps.Add(new Person(\"ann\", 30)); "
        "    for var g in ps.GroupBy(lambda(q) => q.age) do "
        "      Console.WriteLine(\"age \" + g.Key.ToString() + \"=\" + g.Count().ToString()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_TRUE(normalisedStdout(r) == "age 30=2\nage 40=1\n");
}

// ===========================================================================
// THE GROUP'S ELEMENTS — stdlib §5's `IGrouping<TKey, T> : IEnumerable<T>`:
// the element is T (the second generic argument), the first is the key. The
// group's elements are read here, so the element type is pinned.
//
// The selector is FULLY TYPED in this row on purpose: the element half of the
// defect reproduces with a typed selector, so a row that used the un-typed
// spelling would not show that the two halves are independent.
//
// FAILS-WITHOUT: the program is REJECTED — "string member `.age` is not yet
//   supported in codegen" (the element typed as the `string` key).
// PASSES-WITH: "age 30" / "age 40".
// ===========================================================================
BEZEL_TEST(groupby_untyped_lambda_key_selector_6627,
           group_elements_are_the_source_elements) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "record Person(name: string, age: int); "
        "public class Program "
        "  public static method Main(): int "
        "    using var ps := new Array<Person>(); "
        "    ps.Add(new Person(\"ann\", 30)); "
        "    ps.Add(new Person(\"bob\", 40)); "
        "    for var g in ps.GroupBy(lambda(q: Person) => q.name) do "
        "      for var q in g do "
        "        Console.WriteLine(\"age \" + q.age.ToString()); "
        "      end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_TRUE(normalisedStdout(r) == "age 30\nage 40\n");
}

// ===========================================================================
// THE NEGATIVE ROW — a wrong-typed use of `g.Key` is REJECTED. §5.1 admits no
// implicit conversion between `string` and `int`, so `g.Key = 2` (which
// invokes `Equals`, §17.3/§8.4) has no applicable operator.
//
// The diagnostic's OPERAND TYPES are the point: the rejection names `string`
// — the key type the fix derives from the selector's return type.
//
// FAILS-WITHOUT: the §5.1 rejection is NOT reported — the program never
//   reaches it, because on the base the key trampoline is emitted with the
//   ELEMENT's return type and the compile ABORTS in codegen ("Function return
//   type does not match operand type of return inst! ret ptr %4 … malformed IR
//   emitted"). So this row is RED on the base: the diagnostic it asserts is
//   absent there.
// PASSES-WITH: rejected with "between operand types `string` and `int`".
// ===========================================================================
BEZEL_TEST(groupby_untyped_lambda_key_selector_6627,
           wrong_typed_use_of_key_is_rejected) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "record Person(name: string, age: int); "
        "public class Program "
        "  public static method Main(): int "
        "    using var ps := new Array<Person>(); "
        "    ps.Add(new Person(\"ann\", 30)); "
        "    for var g in ps.GroupBy(lambda(q) => q.name) do "
        "      if g.Key = 2 then "
        "        Console.WriteLine(\"eq\"); "
        "      end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "operator `=` has no implicit conversion between operand types "
           "`string` and `int`"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ===========================================================================
// THE ADMITTING TWIN of the negative row — the same `g.Key` used at its own
// type (`g.Key = "ann"`) is ADMITTED. This is the control that shows the
// negative row above is rejected because of the OPERAND TYPES and not because
// the shape is refused wholesale.
//
// It is NOT green on the base: there the key trampoline is emitted with the
// ELEMENT's return type, so the compile ABORTS in codegen before the program
// can run at all. Its value is that on the TIP it runs and prints, which the
// negative row's counterpart cannot do (that program must not run).
//
// FAILS-WITHOUT: `r.ran` is false — the compile aborts in codegen ("malformed
//   IR emitted"), so the program never prints.
// PASSES-WITH: "eq ann".
// ===========================================================================
BEZEL_TEST(groupby_untyped_lambda_key_selector_6627,
           correctly_typed_use_of_key_is_admitted) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "record Person(name: string, age: int); "
        "public class Program "
        "  public static method Main(): int "
        "    using var ps := new Array<Person>(); "
        "    ps.Add(new Person(\"ann\", 30)); "
        "    for var g in ps.GroupBy(lambda(q) => q.name) do "
        "      if g.Key = \"ann\" then "
        "        Console.WriteLine(\"eq ann\"); "
        "      end; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_TRUE(normalisedStdout(r) == "eq ann\n");
}

// ===========================================================================
// THE IDENTITY-KEY CONTROL — `GroupBy(lambda(s) => s)` over `Array<string>`
// is the shape issue #6532 shipped and the existing
// `groupby_string_key_value_equality_6532_test.cpp` pins. It is green on the
// base AND on the tip: the fix must not disturb it.
//
// FAILS-WITHOUT: no — green on the base too.
// PASSES-WITH: "id a=1" / "id bb=1".
// ===========================================================================
BEZEL_TEST(groupby_untyped_lambda_key_selector_6627,
           identity_key_over_string_elements_control) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new Array<string>(); "
        "    a.Add(\"a\"); "
        "    a.Add(\"bb\"); "
        "    for var g in a.GroupBy(lambda(s) => s) do "
        "      Console.WriteLine(\"id \" + g.Key + \"=\" + g.Count().ToString()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors() == false);
    BEZEL_EXPECT_TRUE(normalisedStdout(r) == "id a=1\nid bb=1\n");
}
