// spec §32.11 (v0.333, "Symbol binding: names, aliases, and C-variadic
// imports") — the CODEGEN half of the `:= symbol "name" variadic(N)?` clause.
//
// §32.11: "`variadic(N)` on the symbol clause declares: the named symbol is a
// C-variadic function whose fixed parameters are the first N declared
// parameters; the remaining declared parameters pass in the symbol's variadic
// slots. The Bezel method stays fixed-arity (ordinary overloading, ordinary
// calls); the compiler emits the variadic calling sequence (on SysV x86-64:
// `%al` set correctly)."
//
// These rows LINK AND RUN real calls. They exist because the front-end half
// (tests/sema/extern_symbol_alias_variadic_32_11_test.cpp) and the four
// `s32_11_*` conformance rows are all `lib`-shaped or diagnostic-only: a `lib`
// case compiles with `-c` and never links, so no corpus row and no sema row
// could observe what symbol name the backend actually emitted.

#include <filesystem>
#include <string>

#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// `extern "libc" method open(...)` bound through the clause with BOTH of the
// spec's own declared arities — §32.11's worked example verbatim.
const char kOpenAlias[] =
    "extern \"libc\" "
    "  method Open2(path: CString, flags: int): int "
    "      := symbol \"open\" variadic(2); "
    "  method OpenCreate(path: CString, flags: int, mode: uint): int "
    "      := symbol \"open\" variadic(2); "
    "end; ";

// A file path for the rows below to `open`, owned end to end by THIS process.
//
// The first cut of these rows hardcoded another review run's scratch
// directory. That made them non-hermetic in a way that had nothing to do with
// codegen: `open` needs WRITE permission on the target, and a leftover file
// from an earlier run can be left at mode 0111. Measured — the identical
// program prints False against a mode-0111 leftover and True once the same
// file is writable, so both rows failed on another lane's directory state
// while the compiler was behaving correctly. wtScratchDir gives a per-process
// unique, self-cleaning directory, and unique_stem inside it means no earlier
// run can leave behind a state this one inherits. Per support/scratch_dir.hpp
// this is the ONE sanctioned mechanism; hardcoding a path is what leaked 41 GB
// across the suite once already.
std::string probeFile(const char* stem) {
    return (bezel_test::wtScratchDir("wt-3211-variadic") /
            (::bezel_test::unique_stem(stem) + ".txt"))
        .string();
}

// §32.11 rows read the mode `open` was asked to create a file with. The
// process that runs the program can stat the result directly, so the row
// never has to route through the Bezel program — which matters, because the
// program's own `out` slot cannot hold what `stat` writes (see the
// `open`-mode row below).
std::filesystem::perms createdMode(const std::string& path) {
    std::error_code ec;
    auto st = std::filesystem::status(path, ec);
    return ec ? std::filesystem::perms::none : st.permissions() & std::filesystem::perms::mask;
}

}  // namespace

// ============================================================================
// ALIAS RESOLUTION — the emitted symbol must be the C symbol, not a symbol
// carrying the literal quote characters of the source lexeme.
// ============================================================================

// The alias clause's whole purpose: a Bezel method of one name calling a C
// symbol of another. The pre-fix parser handed the lexeme to LLVM WITH its
// surrounding quotes, so the object file carried an undefined reference
// literally spelled `"getpid"` and every aliased extern failed at link with
// `undefined reference to `"getpid"'` (measured). A `lib`-shaped corpus row
// compiles with `-c` and never links, so this defect was invisible to every
// conformance row the clause fixes.
BEZEL_TEST(extern_variadic_codegen_32_11,
           alias_resolves_to_the_c_symbol_and_links_spec_32_11) {
    auto r = runAot(
        "module Alias; "
        "extern \"libc\" "
        "  method BezelNamedPid(): int := symbol \"getpid\"; "
        "end; "
        "public class Program "
        "  public static method Main() "
        "    var pid := BezelNamedPid(); "
        "    Console.WriteLine(pid > 0); "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("True") != std::string::npos);
}

// §32.11's documented trap in reverse: the SAME C symbol reached through two
// Bezel declarations of DIFFERENT arity — the spec's own `open` pair. Each
// declaration must produce a real call. Pre-fix, both declared the LLVM
// signature with every parameter FIXED, so the two declarations produced
// DIFFERENT function types; the second reused the first via getFunction(),
// and the call site's `args.size() != arg_size()` check then failed — which
// the #2434 backstop turns into a silent `store i32 0`. `open` therefore
// returned 0 and the file was never created.
BEZEL_TEST(extern_variadic_codegen_32_11,
           two_declarations_binding_one_symbol_of_different_arity_spec_32_11) {
    const std::string path = probeFile("two_arity");
    auto r = runAot(
        std::string("module TwoArity; ") + kOpenAlias +
        "public class Program "
        "  public static method Main() "
        "    using var p := CString.From(\"" + path + "\"); "
        // Both calls open with O_CREAT (577), so each returns a valid
        // descriptor whether or not the path already exists. An O_RDONLY
        // second call would succeed or fail purely on whether a PREVIOUS
        // run of this test had created the file, which makes the row pass
        // or fail on directory state rather than on codegen.
        "    var created := OpenCreate(p, 577, 420); "
        "    var opened := Open2(p, 577); "
        "    Console.WriteLine(created > 0); "
        "    Console.WriteLine(opened > 0); "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    // Assert BOTH lines, not `find("True")` — a substring search is
    // satisfied when only one of the two calls survived, which is exactly
    // the half-broken state this row exists to catch.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("True\nTrue") != std::string::npos);
}

// The `mode` argument of §32.11's `OpenCreate` sits at declared index 2, past
// `variadic(2)`, so it passes in the symbol's VARIADIC slot. Reading back the
// file mode proves the callee received it in the register the C prototype
// reads it from — a fixed-slot/vararg-slot mix-up would read a different
// value even where both spellings happen to use the same register.
//
// The read-back is done by the TEST, not by the program, and that is a
// correction rather than a convenience. This row's own first cut declared
// `method StatMode(path: CString, out st: IntPtr): int := symbol "stat";`
// and then never called it — so the row asserted `fd > 0` while its comment
// claimed a mode read-back that did not happen. Completing it inside the
// program is not an option either: `stat` writes a whole `struct stat` (144
// bytes on x86-64) through the `out` pointer, and an `out IntPtr` slot is 8
// bytes. Measured with that declaration actually called: ASan reports
// `stack-buffer-overflow ... WRITE of size 144`, and without ASan the program
// segfaults. So the declaration was both unused and unsafe to use, and it is
// gone; the test process stats the file itself.
BEZEL_TEST(extern_variadic_codegen_32_11,
           variadic_trailing_argument_reaches_the_callee_spec_32_11) {
    const std::string path = probeFile("trailing_arg");
    auto r = runAot(
        std::string("module TrailingArg; ") + kOpenAlias +
        "public class Program "
        "  public static method Main() "
        "    using var p := CString.From(\"" + path + "\"); "
        "    var fd := OpenCreate(p, 577, 384); "   // 384 = 0600
        "    Console.WriteLine(fd > 0); "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("True") != std::string::npos);
    // The load-bearing assertion: 384 == 0600, and `open`'s third argument is
    // read by glibc out of the third INTEGER register. A value delivered to
    // the wrong slot, or not delivered at all, leaves a different mode behind.
    // 0600 survives any ordinary umask (022/002/077 all leave user rw alone),
    // so this asserts the mode, not the process's umask.
    namespace fs = std::filesystem;
    BEZEL_EXPECT_TRUE(createdMode(path) ==
                      (fs::perms::owner_read | fs::perms::owner_write));
}

// A `double` in a variadic position is the `%al` case the spec calls out:
// SysV x86-64 requires the caller to set `%al` to the number of vector
// registers used, and a variadic callee that consults it is undefined
// behaviour when wrong. Routing through `printf`'s `%f` conversion makes the
// value observable — a wrong `%al` (or a float passed without promotion)
// corrupts the read.
BEZEL_TEST(extern_variadic_codegen_32_11,
           double_in_variadic_position_sets_al_spec_32_11) {
    auto r = runAot(
        "module DoubleSlot; "
        "extern \"libc\" "
        "  method Printf(fmt: CString): int "
        "      := symbol \"printf\" variadic(1); "
        "end; "
        "public class Program "
        "  public static method Main() "
        "    using var f := CString.From(\"%.1f\"); "
        "    Printf(f, 2.5); "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("2.5") != std::string::npos);
}