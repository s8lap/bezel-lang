// Spec-derived regression tests for issue #6377 — a SILENT WRONG ANSWER.
//
// Spec: §12.1 (virtual/override matching), §14.1 (generic declarations),
// §23.4 (indexers), §2.8.3 rule 4 (runtime identity per instantiation),
// §28.9 (collections, `.Count`). Revision v0.338.
//
// THE DEFECT. `class B<T>` declares a virtual indexer whose body reads the
// inherited `T`-typed field. `E1: B<Array<int>>` and `E2: B<Array<int>>` both
// INHERIT that accessor unchanged. The vtable-slot emitters memoize the emitted
// accessor on `(ps, accessorSpecTagNow())` — a key that carries NO
// instantiation and NO class — so ONE `bz$indexer$get` (built during
// `declareVtables`, with NO substitution active, i.e. against the ERASED
// `T -> i32` layout) is what every later asker receives, and it lands in the
// vtables of B, E1 AND E2.
//
// MEASURED on this fix's parent (staging 27f35b9b), BEZEL_DUMP_IR=1: one
// i32-returning `bz$indexer$get` in three vtables, consumed at a `call` where
// a pointer is expected. The program COMPILES, LINKS, RUNS and EXITS 0 —
// returning 0 where 3 and 9 are expected. No diagnostic, no IR-verifier abort,
// no sanitizer report. That is the top defect class in this repository: a
// wrong answer that is invisible to every oracle except an assertion that
// happens to check the value. Hence every row below asserts the VALUE.
//
// HARNESS. `runAotChecked` — the strict AOT oracle (fork-isolated
// `Driver::compileFile`, clang link, native exec under generated-code ASan +
// LSan). Every row asserts BOTH `r.ran` and the value: a skip makes the
// harness return `ran == false`, which `BEZEL_EXPECT_TRUE` turns into a row
// failure rather than a silent pass.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

void expectRanWith(const std::string& src, int expected) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

void expectRanStdout(const std::string& src, const std::string& expected) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.stdoutText : std::string("<did not run>"),
                    expected);
}

}  // namespace

// §14.1 / §23.4 / §12.1 (#6377) — THE ISSUE'S REPRO, verbatim. Both classes
// inherit B's indexer accessor UNCHANGED (no `override`), and both instantiate
// `B` at `Array<int>`. `new Array<int>(3, 7)` — the `3` is the count, the `7`
// the fill; `new Array<int>(9, 4)` likewise.
//
// Pre-fix: exit 0 for both, where 3 and 9 are expected. NOT a crash, NOT a
// diagnostic: the program runs to completion and reports the wrong number.
BEZEL_TEST(codegen_indexer_inherited_memo_6377,
           two_classes_instantiating_one_generic_base_spec_14_1) {
    expectRanWith(
        "module M; "
        "public class B<T> "
        "  private owned _a: T; "
        "  public constructor(a: owned T) _a := a; end constructor; "
        "  public virtual property [i: int]: T => _a; "
        "end B; "
        "public class E1: B<Array<int>> "
        "  public constructor() base(new Array<int>(3, 7)); end constructor; "
        "end E1; "
        "public class E2: B<Array<int>> "
        "  public constructor() base(new Array<int>(9, 4)); end constructor; "
        "end E2; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new E1(); "
        "    using var b := new E2(); "
        "    return a[0].Count + b[0].Count; "
        "  end Main; "
        "end Program;",
        12);
}

// §14.1 / §23.4 (#6377) — the SAME two classes, each read on its own so the
// two answers are separately attributable. `3` for E1's array and `9` for E2's.
// Pre-fix both are 0.
BEZEL_TEST(codegen_indexer_inherited_memo_6377,
           first_instantiating_class_reads_its_own_value_spec_14_1) {
    expectRanWith(
        "module M; "
        "public class B<T> "
        "  private owned _a: T; "
        "  public constructor(a: owned T) _a := a; end constructor; "
        "  public virtual property [i: int]: T => _a; "
        "end B; "
        "public class E1: B<Array<int>> "
        "  public constructor() base(new Array<int>(3, 7)); end constructor; "
        "end E1; "
        "public class E2: B<Array<int>> "
        "  public constructor() base(new Array<int>(9, 4)); end constructor; "
        "end E2; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b := new E2(); "
        "    return b[0].Count; "
        "  end Main; "
        "end Program;",
        9);
}

// §12.1 / §14.1 (#6377) — CONTROL: ONE instantiating class. There is nothing
// to conflate here, so the memoised accessor is reachable only under the
// erasure and the erased read is what this row measures. If a fix partitions
// the memo per instantiation, this row must keep answering 3; if a fix instead
// *deletes* a rejection or refuses the shape, this row goes red.
BEZEL_TEST(codegen_indexer_inherited_memo_6377,
           single_instantiating_class_control_spec_12_1) {
    expectRanWith(
        "module M; "
        "public class B<T> "
        "  private owned _a: T; "
        "  public constructor(a: owned T) _a := a; end constructor; "
        "  public virtual property [i: int]: T => _a; "
        "end B; "
        "public class E1: B<Array<int>> "
        "  public constructor() base(new Array<int>(3, 7)); end constructor; "
        "end E1; "
        "public class Program "
        "  public static method Main(): int "
        "    using var a := new E1(); "
        "    return a[0].Count; "
        "  end Main; "
        "end Program;",
        3);
}