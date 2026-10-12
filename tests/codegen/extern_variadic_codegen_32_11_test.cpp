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

#include <string>

#include "support/aot_support.hpp"
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
    auto r = runAot(
        std::string("module TwoArity; ") + kOpenAlias +
        "public class Program "
        "  public static method Main() "
        "    using var p := CString.From("
        "\"/var/tmp/bezel-scratch/r28-3211-review/probes/codegen_3211.txt\"); "
        "    var created := OpenCreate(p, 577, 420); "
        "    var opened := Open2(p, 0); "
        "    Console.WriteLine(created > 0); "
        "    Console.WriteLine(opened > 0); "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    // Both calls must reach libc. A degraded call yields the backstop's
    // literal 0, which fails `> 0`.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("True") != std::string::npos);
}

// The `mode` argument of §32.11's `OpenCreate` sits at declared index 2, past
// `variadic(2)`, so it passes in the symbol's VARIADIC slot. Reading back the
// file mode proves the callee received it in the register the C prototype
// reads it from — a fixed-slot/vararg-slot mix-up would read a different
// value even where both spellings happen to use the same register.
BEZEL_TEST(extern_variadic_codegen_32_11,
           variadic_trailing_argument_reaches_the_callee_spec_32_11) {
    auto r = runAot(
        std::string("module TrailingArg; ") + kOpenAlias +
        "extern \"libc\" "
        "  method StatMode(path: CString, out st: IntPtr): int "
        "      := symbol \"stat\"; "
        "end; "
        "public class Program "
        "  public static method Main() "
        "    using var p := CString.From("
        "\"/var/tmp/bezel-scratch/r28-3211-review/probes/codegen_3211.txt\"); "
        "    var fd := OpenCreate(p, 577, 384); "   // 384 = 0600
        "    Console.WriteLine(fd > 0); "
        "  end Main; "
        "end Program; ");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("True") != std::string::npos);
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