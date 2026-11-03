// The SPAN FAMILY's §22.5 NAME SURFACE — the ONE built-in parameter table's
// (`src/common/BuiltinRegistry.cpp`'s `kBuiltinParams`) rows for stdlib
// §3.3's `ReadOnlyStringSpan`, §4.14's `Span` / `ReadOnlySpan` and §4.15's
// `ArraySpan` / `ReadOnlyArraySpan`.
//
// The issue #7499 reports says: "the registry's SPAN-FAMILY parameter names
// are CAPITALISED (`Buffer` / `Start` / `Length`) while the stdlib declares
// them lowercase — a legal §22.5 spelling is REFUSED".  THE PREMISE IS WRONG,
// and the registry is RIGHT.  The document declares:
//
//   stdlib §3.3:  `public record ReadOnlyStringSpan(Source: string,
//                    Start: int, Length: int)`
//   stdlib §4.14: `public record Span<T>(Buffer: OwningBuffer<T>, Start: int,
//                    Length: int)`
//                 `public record ReadOnlySpan<T>(Buffer: OwningBuffer<T>,
//                    Start: int, Length: int)`
//   stdlib §4.15: `public record ArraySpan<T>(Source: Array<T>, Start: int,
//                    Length: int)`
//                 `public record ReadOnlyArraySpan<T>(Source: Array<T>, Start:
//                    int, Length: int)`
//
// The names are CAPITALISED IN THE DOCUMENT ITSELF, because a record's
// positional parameters ARE its fields (spec A.3 `PositionalField`), and
// spec §1 makes identifiers case-sensitive ("`Foo`, `foo`, and `FOO` are
// distinct").  The lowercased spelling is therefore NOT a legal §22.5
// binding, and refusing it is the CORRECT answer.  (stdlib §1's WINDOW
// convention — "a parameter pair naming a sub-range of a sequence is spelled
// `(start, length)`" — is about a METHOD's parameters; a record's are its
// FIELDS, and the document's own examples read `sp.Length` and
// `Buffer[Start + i]`.)
//
// The half of #7499 that WAS a defect — "THE ARM MUST READ THROUGH THE §22.5
// BINDING … OTHERWISE A REORDERED NAMED CONSTRUCTION IS STILL A SILENT WRONG
// ANSWER" — is fixed by 7b9329c3 (fix #7498): `Emitter::lowerNew`'s span
// arms read the PUBLISHED binding (`common::ctorArgFor`) instead of
// `ne.args[i]`.  The rows below pin it: they FAIL on the pre-#7498 tree
// (the reordered named construction read its arguments by position) and PASS
// on this one.
//
// Oracle: runAot.  The accept rows assert the program's OUTPUT; the reject
// row asserts the SPECIFIC diagnostic (a `Severity::Error` match).
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// A program with a `Main` and the auto-imported modules the rows need.
std::string prog(const std::string& imports, const std::string& body) {
    return "module M;\nimport System;\nimport System.Collections.Generic;\n"
           "import System.IO;\n" + imports +
           "class P\n"
           "  public static method Main(): int\n" + body +
           "\n    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

bool hasError(const bezel::test::JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// The DOCUMENT's names are the legal spellings, and a REORDERED named
// construction binds the parameter each name NAMES (spec §22.5: "Named
// arguments may appear in any order after positional arguments. Each
// supplied argument binds exactly one parameter").  Before 7b9329c3 (fix
// #7498) the span arms of `Emitter::lowerNew` read `ne.args[i]`
// POSITIONALLY, so this construction was a SILENT WRONG ANSWER (it failed
// IR verification outright).
BEZEL_TEST(builtin_span_names, span_reordered_named_binds_by_name) {
    const std::string body =
        "    var a := new OwningBuffer<int>(4);\n"
        "    var s := new Span<int>(Length := 3, Start := 1, Buffer := a);\n"
        "    Console.WriteLine(s.Length);\n"
        "    Console.WriteLine(s.Start);\n";
    auto r = runAot(prog("", body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Length is 3 and Start is 1 — read by position the construction would
    // have put 3 in the BUFFER slot and 1 in the START slot, i.e. a span of
    // length 1 over a buffer of 3.
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n1\n");
}

// The same for the other three members of the family, and for the
// `ReadOnlyStringSpan` of stdlib §3.3.
BEZEL_TEST(builtin_span_names, span_family_reordered_named_binds_by_name) {
    const std::string body =
        "    var a := new OwningBuffer<int>(4);\n"
        "    var arr := new Array<int>(4);\n"
        "    var rs := new ReadOnlySpan<int>(Length := 2, Start := 1, Buffer := a);\n"
        "    var asp := new ArraySpan<int>(Length := 4, Start := 2, Source := arr);\n"
        "    var rasp := new ReadOnlyArraySpan<int>(Length := 1, Start := 3, Source := arr);\n"
        "    var ss := new ReadOnlyStringSpan(Length := 5, Start := 1, Source := \"hello world\");\n"
        "    Console.WriteLine(rs.Length);\n"
        "    Console.WriteLine(asp.Length);\n"
        "    Console.WriteLine(rasp.Length);\n"
        "    Console.WriteLine(ss.Length);\n";
    auto r = runAot(prog("", body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n4\n1\n5\n");
}

// The lowercase spelling is NOT a legal §22.5 binding — the document
// declares `Buffer` / `Start` / `Length` — and it is refused with the
// SHARP diagnostic that NAMES the parameter it does not declare (spec
// §22.5; spec §1's case-sensitivity: "`Foo`, `foo`, and `FOO` are
// distinct").
BEZEL_TEST(builtin_span_names, span_lowercase_parameter_name_is_refused) {
    auto r = runAot(prog("",
        "    var a := new OwningBuffer<int>(4);\n"
        "    var s := new Span<int>(length := 3, start := 0, buffer := a);\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "constructor of built-in `Span`"));
    BEZEL_EXPECT_TRUE(hasError(r, "has no parameter named `length`"));
    BEZEL_EXPECT_TRUE(hasError(r, "(§22.5)"));
    // A §22.5 refusal, not a lowering failure.
    BEZEL_EXPECT_FALSE(hasError(r, "IR verification failed"));
}

// The POSITIONAL spelling still works — the legal spellings are not narrowed
// by the name surface.
BEZEL_TEST(builtin_span_names, span_positional_construction_still_works) {
    const std::string body =
        "    var a := new OwningBuffer<int>(4);\n"
        "    var s := new Span<int>(a, 1, 3);\n"
        "    Console.WriteLine(s.Length);\n"
        "    Console.WriteLine(s.Start);\n";
    auto r = runAot(prog("", body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n1\n");
}
