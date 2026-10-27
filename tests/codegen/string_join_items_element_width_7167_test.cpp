// fix #7167 — `string.Join`'s ITEMS argument is not type-gated, and the lazy
// materialiser that drains it reads the elements as 8-byte POINTER elements
// ⇒ AddressSanitizer: **heap-buffer-overflow, READ of size 8**.
//
// THE DEFECT. stdlib §9.6 (v0.343) declares
//
//   public static method Join(separator: string, items: IEnumerable<string>): string;
//
// — `items` is an `IEnumerable<string>`, so every element is a `string` (one
// pointer, 8 bytes). The `string.Join` codegen arm
// (`src/codegen/LlvmBackend_Calls.cpp`, the static `Join` arm) materializes the
// items sequence through the shared lazy path and then drains it with a
// **hard-coded** element width:
//
//     arr = builder_.CreateCall(toArr, {lazy, builder_.getInt64(8)}, "join.items");
//
// The drain (`bz_lazy_drain_to_array` → `bz_array_add` → `memcpy`) copies that
// many bytes out of the pipeline's current-element buffer, which the runtime
// allocated with the SOURCE's element width. When the source's elements are
// narrower than 8 bytes, the copy runs off the end of that buffer.
//
// MEASURED pre-fix on this tree (each shape compiled with `bezelc
// --sanitize=address`, run under ASan):
//
//   string.Join(",", Array<int>)  →  "ERROR: AddressSanitizer:
//     heap-buffer-overflow ... READ of size 8 ... in memcpy
//     <- bz_array_add (src/runtime/bezel_runtime_collections.cpp:327)
//     <- bz_lazy_drain_to_array (src/runtime/bezel_runtime_collections.cpp:3932)"
//     "0 bytes after 4-byte region", the region allocated by bz_lazy_new:3040
//     (the 4-byte current-element scratch buffer of an `int` element);
//
//   string.Join(",", "ab")        →  the SAME report and the SAME stack, the
//     4-byte region being the `char` element slot of a string source
//     (bz_lazy_from_owned_array).
//
// ⇒ A TYPE GATE ALONE WOULD NOT HAVE CLOSED IT: the plain-string shape is a
// well-typed `string` argument that merely is not an `IEnumerable<string>`,
// and the overflow happens because the DRAIN's copy width is not the width
// the pipeline holds.
//
// THE FIX (this lane): the drain's copy width is bounded by the pipeline's own
// element width (`src/runtime/bezel_runtime_collections.cpp`,
// `bz_lazy_drain_to_array`). A caller that asks for a wider element than the
// pipeline produces gets NO snapshot of those elements — the drain refuses the
// materialization and returns an empty snapshot — so the out-of-bounds read is
// IMPOSSIBLE.
//
// ⇒ THE ORACLE. An out-of-bounds READ can land in mapped memory and exit 0,
// so a row that asserted only an exit code would be VACUOUS. Every row here
// runs under `runAotChecked`, which arms AddressSanitizer over the GENERATED
// module and links the ASan runtime (so the overflow is caught at the
// memcpy) and LeakSanitizer at exit; the harness turns either into an error
// diagnostic and leaves `ran` false. The rows assert the ABSENCE of that
// report — not an exit code.
//
// ⚠ THE PROGRAMS ARE SPELLED `using var`. A `var`-declared array literal
// LEAKS when it is passed to `string.Join` (144 B in 2 allocations: the
// `bz_array_alloc` header at collections.cpp:252 and its backing store at
// collections.cpp:257) because the call makes the array ESCAPE and its
// scope-end release is SKIPPED. That leak is a PRE-EXISTING, SEPARATE defect
// — the same shape as #7166's `string.From` escape (same 144 B / 2
// allocations, same two allocation sites), at a different call site — and it
// is NOT what this file pins. The `using var` spelling does not leak (#7166
// measured that too), and it still reaches the very same drain, so the
// overflow this file pins reproduces identically under it.
//
// ⚠ THESE ROWS ACCEPT EITHER VERDICT ON THE ITEMS ARGUMENT. The PRIMARY fix
// for this defect is the front end: §9.6 declares `items: IEnumerable<string>`,
// so an `Array<int>` (or a bare `string`) is not an admissible argument and
// the call must be REJECTED before it reaches codegen. That gate is a separate
// defect (reported to the coordinator; the sema file belongs to another live
// lane). These rows therefore assert what is true under BOTH verdicts — the
// program never trips the sanitizer — and accept either a front-end rejection
// or a clean run. The backstop they pin (the drain's width bound) is what
// makes the overflow impossible for every element type whose width differs
// from the caller's.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A bare `module M; class C ... end C;` shell — the `string` members are
// compiler intrinsics, so no imports or hierarchy are required.
std::string prog(const std::string& mainBody) {
    return "module M;\nclass C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

// True iff the harness reported a MEMORY-SAFETY failure for this program: a
// sanitizer report (AddressSanitizer / LeakSanitizer) or a runtime crash.
// `runAotChecked` turns either into an error diagnostic and leaves `ran`
// false; the needles below name the harness's own wording.
bool memoryError(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("sanitizer fired") != std::string::npos) return true;
        if (d.message.find("AddressSanitizer") != std::string::npos) return true;
        if (d.message.find("LeakSanitizer") != std::string::npos) return true;
        if (d.message.find("terminated by signal") != std::string::npos) return true;
    }
    return false;
}

// The memory-safety verdict every row below asserts, plus the two guards
// that keep the row from being satisfied for the wrong reason:
//   * a TOOLCHAIN SKIP must be LOUD — a skipped ASan link proves nothing;
//   * the program must have reached a VERDICT: it either ran to completion or
//     the front end rejected it at a real location (the §9.6 items type gate,
//     once it lands).
void expectNoMemoryError(const JitResult& r) {
    BEZEL_EXPECT_FALSE(memoryError(r));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (r.ran)
        BEZEL_EXPECT_EQ(r.exitCode, 0);
    else
        BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

}  // namespace

// ── THE ISSUE'S TWO SHAPES ───────────────────────────────────────────────
//
// MEASURED pre-fix: the ASan report quoted in the file header —
// "heap-buffer-overflow ... READ of size 8 ... in memcpy <- bz_array_add
// (collections.cpp:327) <- bz_lazy_drain_to_array (collections.cpp:3932)",
// "0 bytes after 4-byte region".

// An `Array<int>` items argument — the element width (4) differs from the
// width the drain was told to copy (8).
BEZEL_TEST(codegen_string_join_items_element_width_7167,
           array_int_items_no_out_of_bounds_read) {
    const std::string src = prog(
        "    using var nums := new Array<int>();\n"
        "    nums.Add(1);\n"
        "    nums.Add(2);\n"
        "    nums.Add(3);\n"
        "    Console.WriteLine(string.Join(\",\", nums));\n"
        "    return 0;");
    auto r = runAotChecked(src);
    expectNoMemoryError(r);
}

// A plain `string` items argument — the source's elements are `char`s (4
// bytes each), so this shape is NOT an `Array<int>` in disguise: it is a
// well-typed `string` that merely is not an `IEnumerable<string>`. MEASURED
// pre-fix: the SAME ASan report and the SAME stack.
BEZEL_TEST(codegen_string_join_items_element_width_7167,
           plain_string_items_no_out_of_bounds_read) {
    const std::string src =
        prog("    Console.WriteLine(string.Join(\",\", \"ab\"));\n"
             "    return 0;");
    auto r = runAotChecked(src);
    expectNoMemoryError(r);
}

// ── CONTROLS — the declared shape must still compute the declared value ──
//
// A control that stopped lowering would fail here rather than silently
// returning something else, so every control asserts a VALUE.

// §9.6's declared shape: `items: IEnumerable<string>` over an `Array<string>`.
// The joined string must be byte-identical to what it was before the fix.
BEZEL_TEST(codegen_string_join_items_element_width_7167,
           control_array_string_items_joined_value) {
    const std::string src = prog(
        "    using var parts := new Array<string>();\n"
        "    parts.Add(\"a\");\n"
        "    parts.Add(\"b\");\n"
        "    parts.Add(\"c\");\n"
        "    Console.WriteLine(string.Join(\",\", parts));\n"
        "    return 0;");
    auto r = runAotChecked(src);
    expectNoMemoryError(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "a,b,c\n");
}

// §9.6 declares an `IEnumerable<string>`, NOT a concrete `Array<string>`: the
// lazy-source lowering (#1182) must still work behind the drain. This is the
// path whose element width (8) EQUALS the drain's, so it exercises the
// unchanged side of the drain's width bound.
BEZEL_TEST(codegen_string_join_items_element_width_7167,
           control_lazy_query_items_joined_value) {
    const std::string src =
        "module M; class C public static method Main(): int "
        "  using var xs := new Array<string>(); "
        "  xs.Add(\"a\"); xs.Add(\"b\"); xs.Add(\"c\"); "
        "  var s := string.Join(\",\", xs.Where(lambda(x: string) => "
        "x.Length > 0)); "
        "  Console.WriteLine(s); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAotChecked(src);
    expectNoMemoryError(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "a,b,c\n");
}

// The drain's NORMAL path — a 4-byte element type drained at its own width —
// must still copy the elements' bytes. `ToArray()` over an `Array<int>` is
// the same drain with `esz == 4`; the count proves the copy happened.
BEZEL_TEST(codegen_string_join_items_element_width_7167,
           control_int_elements_still_drain_at_their_own_width) {
    const std::string src = prog(
        "    using var nums := new Array<int>();\n"
        "    nums.Add(1);\n"
        "    nums.Add(2);\n"
        "    nums.Add(3);\n"
        "    using var copy := nums.ToArray();\n"
        "    Console.WriteLine(copy.Count);\n"
        "    return 0;");
    auto r = runAotChecked(src);
    expectNoMemoryError(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}
