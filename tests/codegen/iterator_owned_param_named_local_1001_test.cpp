// Codegen — #1001 / §29.8 / §29.11: a yield iterator's owned parameter is
// disposed exactly once when the IEnumerable<T> source is bound to a named
// local (plain `var`, annotated `var`, or `using var`), not only when iterated
// as a loop temporary.
//
// Background: after #943 introduced source/clone separation (each `for` loop
// obtains a fresh, independent enumerator via GetEnumerator), the loop-temporary
// path disposes the SOURCE frame at loop-end; the named-local path correctly
// skips that to avoid double-disposal and instead relies on
// registerIterableSourceDisposal / registerUsingDisposal to fire at the
// ENCLOSING BLOCK'S CLOSE.
//
// Key observation (§28.3): a `return EXPR` statement evaluates EXPR first, then
// disposes in-scope resources, then returns.  If the source is declared at the
// same level as the `return`, Cell.n is snapshotted BEFORE disposal — this is
// correct spec behaviour, not a leak.  To observe Cell.n == 0 the source must be
// declared in a tighter `do … end` inner scope that closes BEFORE `return Cell.n`
// is reached; all tests below use that pattern.
//
// These tests run the full pipeline including the ownership pass so the
// `iteratorAutoDispose` flag is set and the iterator's Dispose calls
// `disposeOwnedParams`.  Each Cell.n == 0 result means the owned argument was
// disposed exactly once (no leak, no double-free).
//
// §29.8: "The state machine disposes each `owned` parameter in the iterator's
// Dispose, in reverse declaration order, after body cleanup."
//
// BEHAVIOUR FLIP (fix #8787, §29.8 v0.359): an iterable produced by an
// iterator method that declares an `owned` parameter "is single-consumption,
// under exactly §29.11's rule for a lazy query ... a second consumption is a
// compile error where the flow analysis can see it and raises
// `InvalidOperationException` where it is reachable only at run time". The
// three re-enumeration rows below used to enumerate ONE bound local twice
// (§29.11's "each GetEnumerator() call produces an independent enumerator"
// does not reach an owned-parameter iterator any more); that program is now a
// compile error, asserted at the second consumption's line. Each row keeps its
// dispose-exactly-once pin by driving the run-time guard through an untracked
// `IEnumerable<int>` callee (§29.11: "a plain `IEnumerable<T>` parameter is
// statically untracked"), which catches the `InvalidOperationException` and
// still leaves `Cell.n == 0`.
//
// The file runs on runAotChecked (the AOT path with generated-code ASan); the
// deprecated in-process JIT harness skipped definite-assignment, so the new
// static error never fired there and the run-time guard aborted the test
// process instead.

#include <cstddef>
#include <initializer_list>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {
// Full-pipeline runner on the AOT path (generated-code ASan + LSan through
// runAotChecked): ownership and definite-assignment run, so
// `iteratorAutoDispose` is set and a statically visible second consumption of
// a single-consumption iterable is rejected. Returns Main's exit code, or -999
// when the program did not compile or run.
int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// True iff `src` is REJECTED at compile time by a diagnostic at `line` (the
// second consumption) that carries every needle. runAotChecked folds the
// compiler's stderr into one diagnostic whose text holds one
// `prog.bzl:<line>:<col>: <message>` entry per diagnostic, so the line is read
// from that prefix and the needles are searched in that entry alone.
bool rejectedAt(const std::string& src, unsigned line,
                std::initializer_list<const char*> needles) {
    JitResult r = runAotChecked(src);
    if (r.ran) return false;
    const std::string tag = "prog.bzl:" + std::to_string(line) + ":";
    for (const auto& d : r.diagnostics.diagnostics()) {
        for (std::size_t at = d.message.find(tag); at != std::string::npos;
             at = d.message.find(tag, at + 1)) {
            std::size_t end = d.message.find("prog.bzl:", at + tag.size());
            std::string entry = d.message.substr(
                at, end == std::string::npos ? end : end - at);
            bool all = true;
            for (const char* n : needles)
                if (entry.find(n) == std::string::npos) all = false;
            if (all) return true;
        }
    }
    return false;
}

// Shared Cell class: static counter n (ctor +1, dtor -1).
// Cell.n == 0 after `return Cell.n` iff the instance was disposed exactly once
// in the preceding `do … end` block.
const char* kCell =
    "class Cell public static n: int := 0; public V: int := 7; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; ";

// The iterator under test and the class opener: line 1 `module M; <Cell>`,
// line 2 `class P`, lines 3-5 UseCell, so a row's own text starts at line 6 and
// a rejection can name the exact source line of the second consumption.
std::string head(const char* yields = "yield return c.V; yield return c.V + 1;") {
    return std::string("module M; ") + kCell + "\n" +
           "class P\n"
           "  public static method UseCell(owned c: Cell): IEnumerable<int>\n"
           "    " + yields + "\n"
           "  end UseCell;\n";
}
}  // namespace

// §29.8 / §29.11 #1001: plain `var` named local — source declared in a
// `do … end` block so disposal fires before `return Cell.n`.
BEZEL_TEST(iterator_owned_param_named_local_1001, plain_var_named_local_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    do "
        "      var source := P.UseCell(new Cell()); "
        "      for var x in source do end; "
        "    end; "
        "    return Cell.n; "  // 0: source (+ its Cell) disposed when do-block closed
        "  end Main; end P;"), 0);
}

// §29.8 / §29.11 #1001: explicit type annotation — disposal fires at
// the enclosing `do … end` block's close.
BEZEL_TEST(iterator_owned_param_named_local_1001, annotated_var_named_local_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    do "
        "      var source: IEnumerable<int> := P.UseCell(new Cell()); "
        "      for var x in source do end; "
        "    end; "
        "    return Cell.n; "  // 0
        "  end Main; end P;"), 0);
}

// §29.8 / §29.11 #1001: `using var` form — disposal fires at the enclosing
// `do … end` block's close (before `return Cell.n`).
BEZEL_TEST(iterator_owned_param_named_local_1001, using_var_named_local_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    do "
        "      using var source := P.UseCell(new Cell()); "
        "      for var x in source do end; "
        "    end; "
        "    return Cell.n; "  // 0
        "  end Main; end P;"), 0);
}

// §29.11 / §29.8 #1001, flipped by #8787: double enumeration of a named-local
// source. The two `for`s over the bound `source` are a second consumption the
// flow analysis sees, so that program is rejected at the second `for`; the
// Cell is still disposed exactly once when two enumerations run inside an
// untracked callee (the second raises InvalidOperationException).
BEZEL_TEST(iterator_owned_param_named_local_1001, double_enum_named_local_disposed_once) {
    BEZEL_EXPECT_TRUE(rejectedAt(
        head() +
        "  public static method Main(): int\n"            // 6
        "    do\n"                                         // 7
        "      var source := P.UseCell(new Cell());\n"     // 8
        "      for var x in source do end;\n"              // 9
        "      for var x in source do end;\n"              // 10: second consumption
        "    end;\n"
        "    return Cell.n;\n"
        "  end Main; end P;",
        10, {"iterable `source` was already consumed", "`UseCell`", "for-in enumeration"}));
    BEZEL_EXPECT_EQ(run(
        head() +
        "  public static method Twice(src: IEnumerable<int>): int\n"
        "    var caught := 0;\n"
        "    for var x in src do end;\n"
        "    try\n"
        "      for var y in src do end;\n"
        "    catch InvalidOperationException =>\n"
        "      caught := 1;\n"
        "    end;\n"
        "    return caught;\n"
        "  end Twice;\n"
        "  public static method Main(): int\n"
        "    var r := 0;\n"
        "    do\n"
        "      var source := P.UseCell(new Cell());\n"
        "      r := Twice(source);\n"
        "    end;\n"
        "    if Cell.n <> 0 then return 100; end;\n"  // exactly once, no leak
        "    if r <> 1 then return 1; end;\n"          // the second pass raised
        "    return 0;\n"
        "  end Main; end P;"), 0);
}

// §29.11 / §29.8 #1001, flipped by #8787: nested loops over the same
// named-local source. The inner `for` is a second consumption of `source`
// (the outer `for` spent it), rejected at the inner `for`; through an
// untracked callee the inner enumeration raises InvalidOperationException
// while the outer is live, and the Cell is still disposed exactly once.
BEZEL_TEST(iterator_owned_param_named_local_1001, nested_loops_named_local_disposed_once) {
    BEZEL_EXPECT_TRUE(rejectedAt(
        head() +
        "  public static method Main(): int\n"            // 6
        "    do\n"                                         // 7
        "      var source := P.UseCell(new Cell());\n"     // 8
        "      for var x in source do\n"                   // 9
        "        for var y in source do end;\n"            // 10: second consumption
        "      end;\n"
        "    end;\n"
        "    return Cell.n;\n"
        "  end Main; end P;",
        10, {"iterable `source` was already consumed", "`UseCell`", "for-in enumeration"}));
    BEZEL_EXPECT_EQ(run(
        head() +
        "  public static method Nested(src: IEnumerable<int>): int\n"
        "    var caught := 0;\n"
        "    for var x in src do\n"
        "      try\n"
        "        for var y in src do end;\n"
        "      catch InvalidOperationException =>\n"
        "        caught := 1;\n"
        "      end;\n"
        "    end;\n"
        "    return caught;\n"
        "  end Nested;\n"
        "  public static method Main(): int\n"
        "    var r := 0;\n"
        "    do\n"
        "      var source := P.UseCell(new Cell());\n"
        "      r := Nested(source);\n"
        "    end;\n"
        "    if Cell.n <> 0 then return 100; end;\n"
        "    if r <> 1 then return 1; end;\n"
        "    return 0;\n"
        "  end Main; end P;"), 0);
}

// §29.8 / §29.11 #1001, flipped by #8787: early break in the first
// enumeration, then a second enumeration of the same bound local. The break
// does not give the enumeration right back: the second `for` is rejected;
// through an untracked callee the second enumeration raises
// InvalidOperationException after the first (broken-out) enumerator disposed
// the parameter, and the Cell is disposed exactly once.
BEZEL_TEST(iterator_owned_param_named_local_1001, early_break_then_reenum_disposed_once) {
    BEZEL_EXPECT_TRUE(rejectedAt(
        head("yield return c.V; yield return c.V + 1; yield return c.V + 2;") +
        "  public static method Main(): int\n"            // 6
        "    do\n"                                         // 7
        "      var source := P.UseCell(new Cell());\n"     // 8
        "      for var x in source do break; end;\n"       // 9: break after first element
        "      for var x in source do end;\n"              // 10: second consumption
        "    end;\n"
        "    return Cell.n;\n"
        "  end Main; end P;",
        10, {"iterable `source` was already consumed", "`UseCell`", "for-in enumeration"}));
    BEZEL_EXPECT_EQ(run(
        head("yield return c.V; yield return c.V + 1; yield return c.V + 2;") +
        "  public static method BreakThenAgain(src: IEnumerable<int>): int\n"
        "    var caught := 0;\n"
        "    for var x in src do break; end;\n"
        "    try\n"
        "      for var y in src do end;\n"
        "    catch InvalidOperationException =>\n"
        "      caught := 1;\n"
        "    end;\n"
        "    return caught;\n"
        "  end BreakThenAgain;\n"
        "  public static method Main(): int\n"
        "    var r := 0;\n"
        "    do\n"
        "      var source := P.UseCell(new Cell());\n"
        "      r := BreakThenAgain(source);\n"
        "    end;\n"
        "    if Cell.n <> 0 then return 100; end;\n"
        "    if r <> 1 then return 1; end;\n"
        "    return 0;\n"
        "  end Main; end P;"), 0);
}
