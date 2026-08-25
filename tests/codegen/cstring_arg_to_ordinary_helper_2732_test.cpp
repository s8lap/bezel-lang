// spec §32.6 / §28.3 (v0.282) — `CString.From(...)` passed to an ORDINARY
// BEZEL METHOD's `CString` parameter is reclaimed at the §28.3 full-expression
// boundary.
//
// The bug (#2732) is the non-extern twin of #2728: an FFI-buffer temporary
// minted directly in an ordinary-call argument position was never freed.
// §32.6: "The `CString` class is for **caller-owned** strings: those that
// the Bezel side allocated and is responsible for disposing." §28.3
// *Unbound owned temporaries*: "Such a temporary is disposed at the end of
// the full expression in which it is produced, after every use of it within
// that expression has completed."
//
// The fix is `Emitter::isFfiBufferFactoryCall` + `registerFfiExprTemp` +
// `flushFfiExprTemps` (#2728) wired into the ordinary-call argument site in
// `buildOrderedArgs` at `src/codegen/LlvmBackend_Assign.cpp` (~L11824) and
// mirrored in the imported-method arm at `src/codegen/LlvmBackend_Calls2.cpp`
// (~L8984). That wiring is owned by the umbrella #2809 commit (e0ef8ad),
// "stop reading an FFI-boundary buffer as a `%bz.string`, and reclaim the
// one passed to an ordinary call"; #2732 is transitively fixed by that
// commit and the issue was left OPEN only because nobody traced it back.
//
// `runAotChecked` instruments the generated code with AddressSanitizer and
// LeakSanitizer, the strict oracle for ownership-soundness bugs. Pre-fix
// this shape leaked 9280 B / 64 allocations (the issue's measurement on
// 82ad04a); post-fix the program exits 28 with a clean LSan verdict.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Issue #2732's exact reproducer, byte-for-byte: an ordinary Bezel helper
// (`Use`) takes a `CString` parameter, the helper then calls an extern
// (`strlen`), and the caller passes the result of `CString.From(...)`. The
// loop is deliberate — a single such leak is binary-layout dependent for
// LeakSanitizer (a leftover pointer in a register can make the leaked
// block reachable), 64 iterations makes the verdict unconditional.
BEZEL_TEST(codegen_2732_ffi_arg_reclaim, cstring_to_helper_via_extern_spec_32_6) {
    auto r = runAotChecked(
        "module M; "
        "extern \"c\" public method strlen(s: CString): int; end; "
        "class C "
        "  public static method Use(c: CString): int "
        "    return strlen(c); "
        "  end Use; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 64 do "
        "      total := total + Use(CString.From(\"bezel-method-arg-position-abcdefghij\")); "
        "      i := i + 1; "
        "    end; "
        "    return 28; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 28);
}

}  // namespace
