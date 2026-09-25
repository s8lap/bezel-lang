// Regression test for issue #4867.
//
// spec §22.10 (v0.307); stdlib §7.1 `Stream`, §7.4 `TextReader`/`TextWriter`
// (v0.307).
//
// WHAT THE DEFECT WAS. The stdlib metadata registration for `System.IO` did not
// include the abstract classes `Stream`, `TextReader`, and `TextWriter` — only
// concrete subclasses (`FileStream`, `FileTextReader`, `FileTextWriter`) were
// registered. When a program imported `System.IO` and used `Stream`,
// `TextReader`, or `TextWriter` as a type annotation, the type-resolution
// path consulted the stdlib metadata and failed to find the abstract type,
// producing "unresolved type name `Stream`" (and the same for the other two).
//
// THE FIX. BuiltinRegistry.cpp registers `Stream`, `TextReader`, and
// `TextWriter` as known stdlib types, so they are resolved correctly when
// imported via `import System.IO;`.
//
// FAIL-WITHOUT. On the base commit (d437bab4, pre-fix), this test's compile
// fails with:
//   tests/sema/stdlib_stream_metadata_4867_test.cpp.bzl:?:?: unresolved type name `Stream`
// (and similarly for TextReader and TextWriter). With the fix applied, the
// program compiles, links, runs, and returns 0.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Whole program that imports System.IO and uses Stream/TextReader/TextWriter
// as parameter types.  Uses default(Stream) to avoid definite-assignment
// issues with uninitialized locals; the observable is printed to stdout so
// the exit code remains available for the harness to assert.
std::string prog() {
    return "module M; "
           "import System.IO; "
           "class C "
           "  public static method TakeStream(s: Stream): int "
           "    return 1; "
           "  end TakeStream; "
           "  public static method TakeTextReader(r: TextReader): int "
           "    return 2; "
           "  end TakeTextReader; "
           "  public static method TakeTextWriter(w: TextWriter): int "
           "    return 3; "
           "  end TakeTextWriter; "
           "  public static method Main(): int "
           "    Console.WriteLine(TakeStream(default(Stream)) + TakeTextReader(default(TextReader)) + TakeTextWriter(default(TextWriter))); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

} // anonymous namespace

// =====================================================================
// ACCEPT arm — Stream / TextReader / TextWriter are resolved from
// System.IO metadata and the program compiles, links, runs, and returns 0.
// =====================================================================

BEZEL_TEST(sema_stdlib_stream_metadata_4867,
           import_system_io_stream_textreader_textwriter_resolved_spec_22_10) {
    auto r = runAot(prog());
    BEZEL_EXPECT_EQ(r.ran, true);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(prog()), std::string("6"));
}
