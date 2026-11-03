// Regression tests for #8029 -- a `case` scrutinee that is a fresh OWNING
// COLLECTION temporary was disposed on the exception-unwind edge through the
// VTABLE-DISPATCH flag (`unwindOnly`), and a built-in collection has no
// vtable -- so the dispatch was a WILD JUMP.
//
// THE DEFECT. `lowerCaseStmt` (src/codegen/LlvmBackend_Stmts.cpp) and
// `lowerCaseExpr` (src/codegen/LlvmBackend_Exprs2.cpp) capture the case
// scrutinee's fresh owned temporary into an entry alloca and register a
// `Disposal` for it. The entry's EDGE flag was chosen BEFORE its KIND was
// known: `d.unwindOnly = true` ran unconditionally, and only then did the
// kind get picked (`dynamicDispose` for a class instance, `collection` for a
// built-in owning collection). `emitDisposals` tests the UNWIND-ONLY arm
// FIRST (src/codegen/LlvmBackend_Disposal.cpp:9877), so a collection-kind
// entry carrying `unwindOnly` never reaches the collection arm at all: it
// reaches `emitDisposeAndReclaim` -> `emitDynamicDispose`, which reads word 0
// of the value as a Bezel VTABLE and calls slot 2.
//
// A BUILT-IN COLLECTION HAS NO BEZEL VTABLE. `bz_dict_t`'s word 0 is its
// `buckets` pointer (src/runtime/bezel_runtime_collections.cpp:1014) and
// `bz_llist_t`'s word 0 is its `head` (`:2313`), so the "vtable" is a heap
// pointer and the call target is whatever `*(ptr + 16)` happens to hold --
// for a fresh table that is 0, so the program jumps to the null page. That
// is ARBITRARY CODE EXECUTION, not a crash-shaped bug: it is not caught by a
// leak oracle and not caught by "the program still works".
//
// ── THE ORACLE: A CRASH, NOT A COUNT ────────────────────────────────────────
// A wild jump is caught by RUNNING the program and asserting it COMPLETES.
// Every row below therefore asserts `r.ran` (the harness reports a program
// that died on a signal as `ran = false` with a "program terminated by signal
// N" diagnostic), `r.exitCode == 0`, and the exact stdout. The leak oracle
// (LSan's own SUMMARY line, parsed by the harness's `parseLeakSummary`) runs
// on top: the fixed release is the collection's OWN free, so the collection,
// its backing and its owned elements are all reclaimed exactly once.
//
// MEASURED on the PRE-FIX compiler (parent 6f956384), `bezelc
// --sanitize=address` + `ASAN_OPTIONS=detect_leaks=1`:
//
//   case Mk() of _ => throw new Exception("x"); end case;      (statement)
//   (case Mk() of _ => throw new Exception("x"));               (expression)
//   over `owned OwningDictionary<string, Cell>`:
//     "AddressSanitizer: SEGV on unknown address 0x000000000000
//      (pc 0x000000000000)"  -- exit 1
//   over `owned OwningLinkedList<Cell>`: the same SEGV.
//
// The generated code shows the mechanism exactly. PRE-FIX, `main`:
//
//   mov  0x18(%rbx),%r15   ; the scrutinee pointer out of its slot
//   call bz_wait_for_borrows
//   mov  (%r15),%rax       ; <<< WORD 0 read as a VTABLE (a dict's `buckets`)
//   add  $0x10,%rax        ; <<< slot 2 (bytes 16..24)
//   call *(%rax)          ; <<< THE WILD JUMP
//
// POST-FIX the same site reads `call <bz_dict_free>` -- the collection's own
// release, with the §28.11 element/value disposer -- and the only remaining
// indirect calls in `main` are the EXCEPTION object's vtable Dispose, which is
// the legitimate §28.4 dispatch.
//
// ── WHY THE DICT AND THE LINKED LIST, AND NOT THE ARRAY ────────────────────
// The six element-owning collection heads are `OwningArray` /
// `OwningDictionary` / `OwningSet` / `OwningStack` / `OwningQueue` /
// `OwningLinkedList` (stdlib §4.5 / §4.7 / §4.9 / §4.10 / §4.11; the ONE
// registry is `common::isOwningCollectionHead`). Four of them are ARRAY-backed
// (`bz_array_t`), and since #6776 a `bz_array_t`'s word 0 IS the runtime's own
// `bz_array_vtable`, so the wild dispatch happened to land on a real function
// (`bz_array_obj_dispose`) -- a WRONG disposal, not a wild jump. The two
// heads whose headers have NO vtable at word 0 -- `OwningDictionary`
// (`bz_dict_t`) and `OwningLinkedList` (`bz_llist_t`) -- are the ones that
// JUMPED. A row pins each of the six heads anyway: the fix must be the
// family's, not a two-name patch.
//
// ── THE CONTROLS ───────────────────────────────────────────────────────────
//   * a plain USER OBJECT with a vtable must still unwind normally -- its
//     destructor must still run through the vtable on the exception edge;
//   * the NORMAL edge of the `case` construct (no throw) must still reclaim
//     the scrutinee at the construct's completion;
//   * #8009's own shape -- an `owned` owning-collection CHAIN RECEIVER
//     (`Mk().Clear()`) -- must be unchanged: its `releaseOnUnwindOnly`
//     choice is correct and is NOT in scope for #8029.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated -- the sanitizer report
// the AOT harness embeds in the diagnostic message when LSan/ASan fires, and
// the "program terminated by signal N" line when the program died.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// THE ORACLE. THE PROGRAM MUST COMPLETE.
//
// `r.ran` is false when the program terminated on a signal (the harness
// reports it as "program terminated by signal N") or when the sanitizer
// fired -- which is exactly the wild jump's signature. `exitCode == 0` and
// the stdout pin the program's own behaviour. The leak census is read out of
// LSan's own SUMMARY line, so a row that failed can print the NUMBER it
// measured.
void expectCompletesClean(const JitResult& r, const char* out) {
    // The row must NOT have skipped: a skipped row asserts nothing.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (!r.ran) {
        std::cout << "    [ DID NOT COMPLETE ] "
                  << diagnosticText(r).substr(0, 2000) << "\n";
    }
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        diagnosticText(r), leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
}

// THE STATEMENT-POSITION PROGRAM: a fresh owned owning-collection scrutinee
// whose arm body THROWS, so the construct's exception-unwind edge fires the
// registered entry.
std::string statementProgram(const std::string& head,
                              const std::string& typeArgs,
                              const std::string& addStmt) {
    return "module M; import System; import System.Collections.Generic;\n"
           "class Cell\n"
           "  public v: int := 0;\n"
           "end Cell;\n"
           "class C\n"
           "  public static method Mk(): owned " + head + "<" + typeArgs + ">\n"
           "    var d := new " + head + "<" + typeArgs + ">();\n"
           "    " + addStmt + "\n"
           "    return d;\n"
           "  end Mk;\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      case Mk() of\n"
           "        _ => throw new Exception(\"x\");\n"
           "      end case;\n"
           "    catch Exception => Console.WriteLine(0);\n"
           "    end try;\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// THE EXPRESSION-POSITION PROGRAM: the same construct in expression
// position. This is the `lowerCaseExpr` site (src/codegen/
// LlvmBackend_Exprs2.cpp) -- the twin of `lowerCaseStmt`.
//
// ⚠ THE SHAPE MATTERS. The case expression must be an EXPRESSION STATEMENT
// (`(case …);`), not the initializer of a `var`: the ownership pass only
// marks `CaseExpr::disposeOwnedScrutinee` for the expression-statement form
// (src/ownership/Ownership_Checks.cpp:2153), so the `var r := (case …)`
// form never reaches the site at all. (Measured: with the `var` form the
// entry is not registered and the temporary leaks -- a DIFFERENT, pre-existing
// defect, not this one.)
std::string expressionProgram(const std::string& head,
                              const std::string& typeArgs,
                              const std::string& addStmt) {
    return "module M; import System; import System.Collections.Generic;\n"
           "class Cell\n"
           "  public v: int := 0;\n"
           "end Cell;\n"
           "class C\n"
           "  public static method Mk(): owned " + head + "<" + typeArgs + ">\n"
           "    var d := new " + head + "<" + typeArgs + ">();\n"
           "    " + addStmt + "\n"
           "    return d;\n"
           "  end Mk;\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      (case Mk() of\n"
           "        _ => throw new Exception(\"x\")\n"
           "      );\n"
           "    catch Exception => Console.WriteLine(0);\n"
           "    end try;\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ── THE WILD-JUMP ROWS ──────────────────────────────────────────────────────
// Statement position (`lowerCaseStmt`, src/codegen/LlvmBackend_Stmts.cpp).
// PRE-FIX: "AddressSanitizer: SEGV on unknown address 0x000000000000
// (pc 0x000000000000)" -- the jump through the dictionary's `buckets` pointer.
BEZEL_TEST(case_owned_collection_scrutinee_wild_jump_8029,
           statement_owning_dictionary_scrutinee_throw_completes_spec_28_3) {
    expectCompletesClean(
        runAotChecked(statementProgram("OwningDictionary", "string, Cell",
                                       "d.Add(\"k\", new Cell());")),
        "0\n");
}

// Statement position, the OTHER header with no vtable at word 0
// (`bz_llist_t`'s word 0 is its `head`). PRE-FIX: the same SEGV.
BEZEL_TEST(case_owned_collection_scrutinee_wild_jump_8029,
           statement_owning_linkedlist_scrutinee_throw_completes_spec_28_3) {
    expectCompletesClean(
        runAotChecked(statementProgram("OwningLinkedList", "Cell",
                                       "d.Add(new Cell());")),
        "0\n");
}

// Expression position (`lowerCaseExpr`, src/codegen/LlvmBackend_Exprs2.cpp).
// PRE-FIX: the same SEGV, through the same arm.
BEZEL_TEST(case_owned_collection_scrutinee_wild_jump_8029,
           expression_owning_dictionary_scrutinee_throw_completes_spec_28_3) {
    expectCompletesClean(
        runAotChecked(expressionProgram("OwningDictionary", "string, Cell",
                                         "d.Add(\"k\", new Cell());")),
        "0\n");
}

BEZEL_TEST(case_owned_collection_scrutinee_wild_jump_8029,
           expression_owning_linkedlist_scrutinee_throw_completes_spec_28_3) {
    expectCompletesClean(
        runAotChecked(expressionProgram("OwningLinkedList", "Cell",
                                         "d.Add(new Cell());")),
        "0\n");
}

// ── THE OTHER FOUR HEADS OF THE FAMILY ─────────────────────────────────────
// These four are ARRAY-backed, and since #6776 a `bz_array_t`'s word 0 is the
// runtime's own `bz_array_vtable` -- so the wild dispatch landed on a REAL
// function (`bz_array_obj_dispose`) instead of jumping. They are pinned
// anyway: the release must be the collection's OWN free for every head, and
// a fix that only patched the two crashing names would leave the other four
// on the wrong arm.
BEZEL_TEST(case_owned_collection_scrutinee_wild_jump_8029,
           statement_owning_array_scrutinee_throw_completes_spec_28_3) {
    expectCompletesClean(
        runAotChecked(statementProgram("OwningArray", "Cell",
                                       "d.Add(new Cell());")),
        "0\n");
}

BEZEL_TEST(case_owned_collection_scrutinee_wild_jump_8029,
           statement_owning_set_scrutinee_throw_completes_spec_28_3) {
    expectCompletesClean(
        runAotChecked(statementProgram("OwningSet", "Cell",
                                       "d.Add(new Cell());")),
        "0\n");
}

BEZEL_TEST(case_owned_collection_scrutinee_wild_jump_8029,
           statement_owning_stack_scrutinee_throw_completes_spec_28_3) {
    expectCompletesClean(
        runAotChecked(statementProgram("OwningStack", "Cell",
                                       "d.Add(new Cell());")),
        "0\n");
}

BEZEL_TEST(case_owned_collection_scrutinee_wild_jump_8029,
           statement_owning_queue_scrutinee_throw_completes_spec_28_3) {
    expectCompletesClean(
        runAotChecked(statementProgram("OwningQueue", "Cell",
                                       "d.Add(new Cell());")),
        "0\n");
}

// ── CONTROL 1: A PLAIN USER OBJECT WITH A VTABLE STILL UNWINDS NORMALLY ────
// The class arm of the fix is byte-identical to the pre-fix arm: the
// scrutinee is a heap class instance, its `unwindOnly` entry fires on the
// exception edge, and the DESTRUCTOR RUNS through the vtable. The row pins
// the destructor output ("cell disposed"), so a fix that dropped the class
// arm -- or turned the class's release into a leak -- would go red here.
BEZEL_TEST(case_owned_collection_scrutinee_wild_jump_8029,
           control_user_class_scrutinee_destructor_still_runs_spec_28_4) {
    expectCompletesClean(runAotChecked(R"BZL(
module M; import System;
class Cell
  public v: int := 0;
  destructor
    Console.WriteLine("cell disposed");
  end destructor;
end Cell;
class C
  public static method Main(): int
    try
      case new Cell() of
        _ => throw new Exception("x");
      end case;
    catch Exception => Console.WriteLine(0);
    end try;
    return 0;
  end Main;
end C;
)BZL"),
                         "cell disposed\n0\n");
}

// ── CONTROL 2: THE NORMAL EDGE STILL RECLAIMS THE SCRUTINEE ────────────────
// No throw: the construct completes, and the scrutinee is reclaimed at the
// construct's completion (the K8 full-expression boundary, §28.3). The fix
// changed the EXCEPTION edge only; this row is green before AND after, and it
// is the row that would go red if the fix had made the entry fire on the
// normal edge as well (a double free).
BEZEL_TEST(case_owned_collection_scrutinee_wild_jump_8029,
           control_normal_edge_reclaims_the_scrutinee_spec_28_3) {
    expectCompletesClean(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningDictionary<string, Cell>
    var d := new OwningDictionary<string, Cell>();
    d.Add("k", new Cell());
    return d;
  end Mk;
  public static method Main(): int
    case Mk() of
      _ => Console.WriteLine(1);
    end case;
    return 0;
  end Main;
end C;
)BZL"),
                         "1\n");
}

// ── CONTROL 3: #8009's OWN SHAPE IS UNCHANGED ──────────────────────────────
// The `owned` owning-collection CHAIN RECEIVER: `Mk().Clear()` -- the
// unbound owned temporary is a chain root, disposed at the §28.3 K8.1
// full-expression boundary through the COLLECTION'S OWN release arm
// (`releaseOnUnwindOnly`). #8009's choice is correct and is NOT in scope for
// #8029; this row is green before AND after, and it is the row that would go
// red if the fix had reached into that registration.
//
// 100 iterations, so a one-shot leak repro would be a vacuous oracle.
BEZEL_TEST(case_owned_collection_scrutinee_wild_jump_8029,
           control_8009_owning_collection_chain_receiver_unchanged_spec_28_3) {
    expectCompletesClean(runAotChecked(R"BZL(
module M; import System; import System.Collections.Generic;
class Cell
  public v: int := 0;
end Cell;
class C
  public static method Mk(): owned OwningArray<Cell>
    var a := new OwningArray<Cell>();
    a.Add(new Cell());
    return a;
  end Mk;
  public static method Main(): int
    for var i in 0..<100 do
      Mk().Clear();
    end for;
    Console.WriteLine(0); return 0;
  end Main;
end C;
)BZL"),
                         "0\n");
}
