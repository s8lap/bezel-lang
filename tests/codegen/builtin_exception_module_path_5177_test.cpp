// Codegen — fix #5177 (stdlib §2.13/§2.17, §4.8, §6, §7.5; spec §8.5/§31.4,
// v0.310): `BuiltinRegistry`'s module-path table rendered every built-in
// exception as `System.<Name>` unconditionally. Ten exceptions are declared
// outside `System` per the stdlib's own section structure:
//   - the five §7.5 IO exceptions (`IOException`, `FileNotFoundException`,
//     `DirectoryNotFoundException`, `EndOfStreamException`,
//     `PathTooLongException`) live in `System.IO`;
//   - `ChannelClosedException`, `ChannelFullException`,
//     `DomainFaultedException`, `TimeoutException` (stdlib §6) live in
//     `System.Threading`;
//   - `KeyNotFoundException` (stdlib §4.8) lives in
//     `System.Collections.Generic`.
//
// `Exception.ToString()` (spec §8.5/§31.4, v0.310) renders the receiver's
// qualified type name first, so this is directly observable at runtime:
// `new TimeoutException().ToString()` must start with
// "System.Threading.TimeoutException", not "System.TimeoutException".
//
// FAIL-WITHOUT: with the pre-fix table (every row "System"), this test's
// EXPECT_EQ on the qualified module-path prefix fails, because the runtime
// prints "System.TimeoutException" / "System.IOException" /
// "System.KeyNotFoundException" instead.

#include "support/aot_support.hpp"   // runAot — the strict AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& importLine, const std::string& newExpr) {
    return "module M; " + importLine +
           "class C public static method Main() "
           "using var e := " + newExpr + "; "
           "Console.WriteLine(e.ToString()); "
           "end Main; end C;";
}

}  // namespace

BEZEL_TEST(builtin_exception_module_path_5177, timeout_exception_is_system_threading) {
    auto r = runAot(
        prog("import System.Threading; ", "new TimeoutException()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("System.Threading.TimeoutException\n"));
}

BEZEL_TEST(builtin_exception_module_path_5177, channel_closed_exception_is_system_threading) {
    auto r = runAot(
        prog("import System.Threading; ", "new ChannelClosedException()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                     std::string("System.Threading.ChannelClosedException\n"));
}

BEZEL_TEST(builtin_exception_module_path_5177, channel_full_exception_is_system_threading) {
    auto r = runAot(
        prog("import System.Threading; ", "new ChannelFullException()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                     std::string("System.Threading.ChannelFullException\n"));
}

BEZEL_TEST(builtin_exception_module_path_5177, domain_faulted_exception_is_system_threading) {
    auto r = runAot(
        prog("import System.Threading; ", "new DomainFaultedException()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                     std::string("System.Threading.DomainFaultedException\n"));
}

BEZEL_TEST(builtin_exception_module_path_5177, io_exception_is_system_io) {
    auto r = runAot(prog("import System.IO; ", "new IOException()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("System.IO.IOException\n"));
}

BEZEL_TEST(builtin_exception_module_path_5177, file_not_found_exception_is_system_io) {
    auto r = runAot(prog("import System.IO; ", "new FileNotFoundException()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("System.IO.FileNotFoundException\n"));
}

BEZEL_TEST(builtin_exception_module_path_5177,
           directory_not_found_exception_is_system_io) {
    auto r =
        runAot(prog("import System.IO; ", "new DirectoryNotFoundException()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                     std::string("System.IO.DirectoryNotFoundException\n"));
}

BEZEL_TEST(builtin_exception_module_path_5177, end_of_stream_exception_is_system_io) {
    auto r = runAot(prog("import System.IO; ", "new EndOfStreamException()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("System.IO.EndOfStreamException\n"));
}

BEZEL_TEST(builtin_exception_module_path_5177, path_too_long_exception_is_system_io) {
    auto r = runAot(prog("import System.IO; ", "new PathTooLongException()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("System.IO.PathTooLongException\n"));
}

BEZEL_TEST(builtin_exception_module_path_5177,
           key_not_found_exception_is_system_collections_generic) {
    auto r = runAot(
        prog("import System.Collections.Generic; ", "new KeyNotFoundException()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(
        r.stdoutText,
        std::string("System.Collections.Generic.KeyNotFoundException\n"));
}

// Sanity: a genuinely `System`-declared exception is unaffected by this fix.
BEZEL_TEST(builtin_exception_module_path_5177, argument_exception_stays_system) {
    auto r = runAot(prog("", "new ArgumentException()"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("System.ArgumentException\n"));
}
