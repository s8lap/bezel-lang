// fix #7160 (§27.6 / stdlib §5, v0.343) — a lazy §5 operator whose CALLBACK
// is a METHOD REFERENCE was mis-lowered: the §5 operator sized its
// type-erased element slot from the callback's RESULT TYPE, and a BARE
// method-group identifier (`Select(Identity)`) carries NO Function TypeRef —
// sema types the QUALIFIED spelling (`Select(C.Identity)`) but leaves the bare
// one unset (fix #4897, v0.307, synthesized the missing type only inside
// `checkAssignable`, so it never reached the expression). With no result type
// to read, the §5 lowering fell back to `i32` and the lazy element slot was
// sized 4 bytes for an 8-byte `string` projection.
//
// The consumer then read the element as a `string` — an 8-byte pointer load
// out of a 4-byte allocation ⇒ ASan: heap-buffer-overflow, READ of size 8,
// `0 bytes after 4-byte region`, allocated at `bz_lazy_new`
// (`e->cur = malloc(esz)`) — a MEMORY-SAFETY defect in a program the compiler
// ACCEPTED. The trampoline also called the closure with an `i32` result type,
// truncating the returned string pointer.
//
// ROOT CAUSE. `src/codegen/LlvmBackend_Calls3.cpp`:
//   - `linqSelectorResultType` recovers a callback's result type: the
//     expression's own Function TypeRef when it has one (a lambda; the
//     qualified method-reference spelling), otherwise the DECLARED result of
//     the method the reference names (§27.6's method reference), which is what
//     the bare spelling resolves to.
//   - `argIsFreshClosureCell` — the predicate that decides whether the
//     enumerator OWNS the callback cell — now also recognises an ordinary
//     method reference. §27.6 forms it as a fresh +1 closure cell
//     (`makeMethodThunk` -> `buildMethodClosureCell` callocs one cell per
//     reference, §27.6 / #1170), exactly like an inline lambda literal; the
//     enumerator must own it and release it at dispose. Without that the cell
//     leaked (64 B per operator call) — the leak this file's programs would
//     otherwise trip under the LSan-armed oracle, which is why the row below
//     asserts the memory-safety property under `runAotChecked`.
//
// The spelling is ADMITTED BY THE SPEC: stdlib §5's `Select<U>(selector:
// (T) -> U)` takes a function VALUE, and §27.6 is the section that forms one
// — so a method reference is a legal selector and the program is well-formed.
// The COMPILER was wrong to mis-lower it; the fix does NOT reject the shape.
//
// Fail-without / passes-with: on the base (the staging tip) the §5 method-
// reference selector programs compiled CLEAN and then read out of bounds —
// the row's `runAotChecked` net reports the heap-buffer-overflow (and the
// 64 B closure-cell leak), so the row is RED; with the fix the same programs
// run clean and return the expected value.
//
// Spec: language spec §27.6 (*Method references*), stdlib §5 (`System.
// Collections.Linq` — the deferred operators and their callback types),
// §28.9 / §28.3 (the closure cell's ownership), v0.343.

#include <cstdio>
#include <string>
#include <string_view>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The outcome of one program under the LSan/ASan-armed AOT oracle.
//
// The MEMORY-SAFETY property is asserted DIRECTLY (`sanitizerFired`), not
// through the exit code: an out-of-bounds READ can land in mapped memory and
// exit 0, so a row that only checked the exit code would pass on the base.
struct Checked {
    bool ran = false;             // the program ran to completion
    int exitCode = -1;            // its exit status (8-bit truncated)
    bool rejected = false;        // the compiler rejected the program
    bool sanitizerFired = false;  // the sanitizer net reported (OOB / leak / UAF)
    std::string report;           // the sanitizer report, for the failure log
};

Checked runChecked(const std::string& src) {
    Checked c;
    auto r = runAotChecked(src);
    c.ran = r.ran;
    c.exitCode = r.exitCode;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) {
            c.sanitizerFired = true;
            c.report = d.message;
        }
        // A compile-time rejection is reported as an error diagnostic too; it
        // is not a sanitizer finding, but the program never ran.
        if (d.message.find("sanitizer fired") == std::string::npos)
            c.rejected = true;
    }
    return c;
}

// The row's verdict: the program compiled, ran, produced NO sanitizer report,
// and returned the expected value.
void expectClean(const std::string& src, int expected) {
    auto c = runChecked(src);
    if (c.rejected) {
        std::fprintf(stderr, "[7160] the program was REJECTED at compile time\n");
    }
    if (c.sanitizerFired) {
        std::fprintf(stderr, "[7160] sanitizer report:\n%s\n", c.report.c_str());
    }
    BEZEL_EXPECT_FALSE(c.rejected);
    BEZEL_EXPECT_FALSE(c.sanitizerFired);
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, expected);
}

}  // namespace

// An `int` projection over an `int` source: the element slot is 4 bytes and
// the trampoline calls the closure with an `i32` result — the shape that
// already worked, pinned so the fix does not change it.
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           select_bare_method_reference_int_projection) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method Identity(i: int): int => i; "
        "  public static method Main(): int "
        "    using var nums: Array<int> := [1, 2, 3]; "
        "    var total: int := 0; "
        "    for var n in nums.Select(Identity) do "
        "      total := total + n; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectClean(src, 6);  // 1 + 2 + 3
}

// The same projection with the QUALIFIED spelling (`C.Len`) — the shape that
// already carried a Function TypeRef. It must keep working, byte-identical
// output (3 -> 6), and it must not leak.
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           select_qualified_method_reference_projection) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method Len(s: string): int => s.Length; "
        "  public static method Main(): int "
        "    using var words: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "    var total: int := 0; "
        "    for var w in words.Select(C.Len) do "
        "      total := total + w; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectClean(src, 6);
}

// The LAMBDA spelling — the control that already worked. Its output must not
// change.
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           select_lambda_projection_unchanged) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  public static method Main(): int "
        "    using var words: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "    var total: int := 0; "
        "    for var w in words.Select(lambda (s) => s.Length) do "
        "      total := total + w; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectClean(src, 6);
}

// ===========================================================================
// THE PIN — #7160: a `Select` whose selector is a BARE method reference.
// Before the fix: accepted, then an ASan heap-buffer-overflow (READ of size
// 8) out of the 4-byte element slot, plus a 64 B closure-cell leak.
// After the fix: the element slot is sized from the method's declared result
// type, and the enumerator owns the closure cell.
// ===========================================================================
// The issue's own repro: a `string` -> `string` projection. The element slot
// must be 8 bytes (a pointer), not the 4-byte `i32` fallback — the read that
// overflowed was the consumer's 8-byte pointer load.
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           select_bare_method_reference_string_projection) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method Identity(s: string): string => s; "
        "  public static method Main(): int "
        "    using var words: Array<string> := [\"a\", \"bb\"]; "
        "    var total: int := 0; "
        "    for var w in words.Select(Identity) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectClean(src, 3);  // 1 + 2
}

// A BOOLEAN projection — a one-byte element. The slot must be sized 1 byte
// and the trampoline must call the closure with an `i1` result, exactly as
// the qualified spelling does.
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           select_bare_method_reference_bool_projection) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method Keep(b: bool): bool => b; "
        "  public static method Main(): int "
        "    using var flags: Array<bool> := [true, false, true]; "
        "    var total: int := 0; "
        "    for var f in flags.Select(Keep) do "
        "      if f then total := total + 1; end if; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectClean(src, 2);
}

// The BOUND instance-method spelling (`c.Identity`) — the method reference
// captures its receiver. The projection's result type is recovered the same
// way, and the closure cell (which owns a BORROW of the receiver) is released
// at dispose without touching the receiver.
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           select_bound_instance_method_reference_projection) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private method Identity(s: string): string => s; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    using var words: Array<string> := [\"a\", \"bb\"]; "
        "    var total: int := 0; "
        "    for var w in words.Select(c.Identity) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectClean(src, 3);  // 1 + 2
}

// §5's `Where(predicate: (T) -> bool)` with a bare method reference — the
// sibling operator in the same family. Its callback's result type is a
// predicate, not a projection, so the element slot keeps the SOURCE element
// size; the row pins that the family does not regress there.
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           where_bare_method_reference_predicate) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method NonEmpty(s: string): bool => s.Length > 0; "
        "  public static method Main(): int "
        "    using var words: Array<string> := [\"a\", \"bb\", \"\"]; "
        "    var total: int := 0; "
        "    for var w in words.Where(NonEmpty) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectClean(src, 3);  // 1 + 2
}

// §5's `OrderBy<TKey>(key: (T) -> TKey)` with a bare method reference.
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           orderby_bare_method_reference_key) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method Len(s: string): int => s.Length; "
        "  public static method Main(): int "
        "    using var words: Array<string> := [\"bb\", \"a\", \"ccc\"]; "
        "    var total: int := 0; "
        "    for var w in words.OrderBy(Len) do "
        "      total := total + w.Length; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectClean(src, 6);  // 1 + 2 + 3
}

// ===========================================================================
// THE SLOT-WIDTH MATRIX — the element slot must be as wide as the value the
// CONSUMER reads out of it.
//
// A bare method reference carries no Function TypeRef (#4897), so sema still
// types the operator's result as the SOURCE element type (stdlib §5's
// `Select<U>`), while the selector's own result type is the method's
// declared one. The two widths differ, and the slot has to satisfy BOTH: the
// selector writes its own width into it, the terminal drain reads sema's
// element width out of it. The rows below pin the four width combinations.
// ===========================================================================

// int source, `bool` result: the selector writes ONE byte, the consumer reads
// the source element's FOUR bytes. A slot sized from the selector's result
// (1 byte) is an out-of-bounds READ of size 4 by the drain. The projected
// value is not used — sema types the result as `int`, so the value is
// mistyped by sema, not by the compiler (a separate sema gap, #7164).
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           select_bare_method_reference_int_source_bool_result) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method IsPos(i: int): bool => i > 0; "
        "  public static method Main(): int "
        "    using var nums: Array<int> := [1, 2, 3]; "
        "    var total: int := 0; "
        "    for var n in nums.Select(IsPos) do "
        "      total := total + 1; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectClean(src, 3);
}

// `long` source, `long` result: both widths are EIGHT bytes. The 4-byte `i32`
// fallback slot was an out-of-bounds READ of size 8. The projected value is
// used, so this row also pins that the value survives the projection.
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           select_bare_method_reference_long_projection) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method IdLong(v: long): long => v; "
        "  public static method Main(): int "
        "    using var nums: Array<long> := [1L, 2L, 3L]; "
        "    var total: long := 0L; "
        "    for var n in nums.Select(IdLong) do "
        "      total := total + n; "
        "    end for; "
        "    if total = 6L then return 0; end if; "
        "    return 1; "
        "end Main; "
        "end C;";
    expectClean(src, 0);
}

// `string` source, `int` result: the selector writes FOUR bytes, the consumer
// reads the source element's EIGHT bytes — the slot must be eight wide. The
// projected value is not used (sema types the result as `string`).
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           select_bare_method_reference_string_source_int_result) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method Len(s: string): int => s.Length; "
        "  public static method Main(): int "
        "    using var words: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "    var total: int := 0; "
        "    for var w in words.Select(Len) do "
        "      total := total + 1; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectClean(src, 3);
}

// `int` source, `string` result: the selector writes EIGHT bytes, the
// consumer reads the source element's FOUR bytes — the slot must be eight
// wide for the selector's write, or the write itself overflows. The
// projected value is not used (sema types the result as `int`).
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           select_bare_method_reference_int_source_string_result) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method ToStr(i: int): string => \"x\"; "
        "  public static method Main(): int "
        "    using var nums: Array<int> := [1, 2, 3]; "
        "    var total: int := 0; "
        "    for var n in nums.Select(ToStr) do "
        "      total := total + 1; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectClean(src, 3);
}

// §5's `Zip<U,R>(other, combine: (T,U) -> R)` with a bare method reference as
// the COMBINER — the sibling operator with the same slot-width rule: the
// combiner writes its result width into the slot, and the consumer reads
// sema's element width out of it. A `string` source with an `int` combiner
// leaves the two four bytes apart, so a slot sized from the combiner's
// result alone is an out-of-bounds READ of size 8.
BEZEL_TEST(lazy_select_method_reference_selector_7160,
           zip_bare_method_reference_narrow_combiner_result) {
    std::string src =
        "module M; import System.Collections.Linq; "
        "class C "
        "  private static method Pick(a: string, b: string): int => 1; "
        "  public static method Main(): int "
        "    using var xs: Array<string> := [\"a\", \"bb\"]; "
        "    using var ys: Array<string> := [\"c\", \"dd\"]; "
        "    var total: int := 0; "
        "    for var w in xs.Zip(ys, Pick) do "
        "      total := total + 1; "
        "    end for; "
        "    return total; "
        "end Main; "
        "end C;";
    expectClean(src, 2);
}
