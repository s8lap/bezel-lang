// Regression test for issue #9152 — a VIRTUAL indexer whose SETTER body is an
// EMPTY block (`set => do end;`) SEGV'd at address 0x0 when written through,
// because the vtable's setter slot was never populated: the call went through
// a NULL function pointer.
//
// THE DEFECT. The issue's own repro:
//
//   public class Fl
//     private _v: int := 0;
//     public virtual property [i: int]: int
//       get => _v;
//       set => do end;
//     end;
//     public constructor() end constructor;
//   end Fl;
//   // ...
//   fl[0] := 5;               // SIGSEGV, pc = 0x0
//
// The program compiled with NO diagnostic at any severity and died at run time
// with "SEGV on unknown address 0x000000000000" — a call through a NULL
// vtable slot.
//
// THE ROOT CAUSE. `Emitter::ensureIndexerSetterFn`
// (src/codegen/LlvmBackend_Assign.cpp) decided "does this indexer have a
// setter body?" with
//
//     for (const auto& a : pd->accessors) {
//         if (a.isSet) {
//             if (a.exprBody) setterExpr = a.exprBody.get();
//             else if (!a.stmts.empty()) setterStmts = &a.stmts;
//             break;
//         }
//     }
//     if (!setterExpr && !setterStmts) return nullptr;
//
// `!a.stmts.empty()` is a PROXY for "this accessor has a body", and the proxy
// is wrong for a block accessor whose block is EMPTY: `do end` parses to an
// accessor with `bodyKind == Block` and an EMPTY statement vector. So the
// emitter concluded "this indexer has no setter at all" and returned nullptr;
// `declareVtables` Pass D then left the slot at its `ConstantPointerNull`
// initialiser (`if (fn) elems[...] = fn;`), and `lowerIndexerSet`'s class-vtable
// branch loaded that NULL and CALLED it.
//
// The two sibling paths never made that mistake, which is exactly why the
// issue's twins are clean: the indexer GETTER twin
// (`ensureIndexerGetterFn`) keys on `a.bodyKind ==
// ast::PropertyAccessorBodyKind::Block`, and the NAMED-property twin
// (`ensurePropertyAccessorFn`) keys on `a.bodyKind !=
// ast::PropertyAccessorBodyKind::Abstract`. The accessor AST carries the
// authoritative discriminator (src/ast/Node.hpp: `PropertyAccessorBodyKind {
// Expression, Block, Abstract }`).
//
// THE SPEC. Grammar A.4 (`docs/Bezel_Language_Spec.md`):
// `AccessorBody ::= "=>" (Expression | AssignmentStmt | "do" Statement* "end")
// ";"` — `Statement*` admits ZERO statements, so `set => do end;` is a legal
// accessor body. §23.3 (v0.359) *Block-form properties*: "Each accessor uses
// `=> expression` or `=> do ... end`", and, decisively, a store through such a
// setter "is judged at the site … whatever the setter's body does with
// `value`" — the spec explicitly contemplates a setter body that does nothing
// with `value`. §12.5 (v0.359) *Abstract classes and members*: "An abstract
// member ends with `;` and has no `end`" — so `do end` is NOT abstract; it is
// a CONCRETE accessor whose body does nothing. §23.4 (v0.359) *Indexers*:
// "They participate in `virtual`/`override`/`sealed`/`abstract` like methods" —
// so the write dispatches dynamically to that concrete accessor. §22.1
// (v0.359): "A method body may be **empty** … a procedure with an empty body
// completes immediately" — the empty body is a first-class shape.
//
// So the spec DECIDES the case: `set => do end;` on a virtual indexer is a
// setter whose body does nothing. The write must dispatch to it (a no-op), not
// to a null slot. The fix emits that accessor exactly as a non-empty one is
// emitted — the accessor EXISTS, its body is empty.
//
// ROWS.
//   Row 1 — the issue's own repro: a VIRTUAL indexer, an EMPTY setter body,
//           200 writes. RED on the base (SIGSEGV at 0x0), GREEN with the fix.
//   Row 2 — the OVERRIDE twin: the receiver's STATIC type is the BASE class,
//           so the write really goes through the vtable slot to a DERIVED
//           class's override whose setter body is empty. RED on the base
//           (SIGSEGV at 0x0), GREEN with the fix. The printed value is an
//           independent oracle that the OVERRIDE's empty setter ran (1) and
//           not the base's non-empty one (6).
//   Row 3 — the ASAN leg: the same shape under the checked oracle, with a
//           LAMBDA index argument (the issue's second measured spelling), 200
//           iterations. RED on the base (SIGSEGV under ASan), GREEN with the
//           fix — and LeakSanitizer must count nothing, so the fix may not
//           trade the crash for a leak.
//   Row 4 — GREEN CONTROL: the non-empty-setter twin the issue names
//           (`set => do _v := value; end;`). Green before and after; it fails
//           if the fix disturbs an ordinary virtual indexer setter.
//
// per §23.3, §23.4, §12.5, §21.6, §22.1 (issue #9152)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program ran to completion: it compiled, it exited normally with the
// program's own exit code, and it printed exactly the expected output. A
// SIGSEGV is reported by the harness as `ran == false` with exit code -1 and
// an Error diagnostic ("program terminated by signal 11"), so the `ran` and
// the stdout assertions are what make this row RED on the base.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The checked (ASan/LSan) oracle's verdict on the same program.
void expectCleanChecked(const JitResult& r, int exitCode,
                        const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    expectClean(r, exitCode, out);
    assertNoLeak(r);
}

// ── Row 1 (the issue's own repro / §23.3 / §23.4): a VIRTUAL indexer whose
// SETTER body is an EMPTY block. 200 writes, so a per-write defect cannot
// hide. The program compiles with no diagnostic on the base — the defect is
// purely a run-time one.
//
// RED on the base (SIGSEGV, pc = 0x0 — a call through the NULL vtable slot),
// GREEN with the fix: the accessor EXISTS (an empty body), the write is a
// no-op, and the program prints its own marker.
BEZEL_TEST(virtual_indexer_empty_setter_body_9152,
           issue_repro_virtual_indexer_empty_setter_body_does_not_crash) {
    const std::string src =
        "module M; "
        "import System; "
        "public class Fl "
        "  private _v: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _v; "
        "    set => do end; "
        "  end; "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    using var fl := new Fl(); "
        "    for var j in 0..<200 do "
        "      fl[0] := 5; "
        "    end; "
        "    Console.WriteLine(0); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAot(src), 0, "0\n");
}

// ── Row 2 (§12.1 / §23.4): the OVERRIDE twin. The receiver's STATIC type is
// the BASE class, so the write dispatches through the vtable slot to the
// DERIVED class's override — whose setter body is empty. The BASE's setter
// body is NOT empty (`do _v := value; end;`), so the printed value says which
// accessor ran: the derived override's empty setter leaves `_v` at 0 and the
// derived getter returns `_v + 1` = 1; had the dispatch wrongly reached the
// BASE's setter, `_v` would be 5 and the print would be 6.
//
// RED on the base (SIGSEGV, pc = 0x0 — the derived class's own vtable slot is
// the NULL one), GREEN with the fix.
BEZEL_TEST(virtual_indexer_empty_setter_body_9152,
           override_twin_dispatches_to_the_empty_override) {
    const std::string src =
        "module M; "
        "import System; "
        "public class Fl "
        "  protected _v: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Fl2 : Fl "
        "  public override property [i: int]: int "
        "    get => _v + 1; "
        "    set => do end; "
        "  end; "
        "  public constructor() end constructor; "
        "end Fl2; "
        "public class Program "
        "  public static method Main(): int "
        "    using var fl: Fl := new Fl2(); "
        "    for var j in 0..<200 do "
        "      fl[0] := 5; "
        "    end; "
        "    Console.WriteLine(fl[0]); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAot(src), 0, "1\n");
}

// ── Row 3 (§28.14 / §28.9 / §27.5): the issue's SECOND measured spelling —
// the index argument's TYPE is irrelevant — a LAMBDA written at the index
// position, under the CHECKED oracle (AddressSanitizer over the generated
// module + LeakSanitizer at exit). The lambda is a +1 closure producer
// (§28.3); §23.5 forbids `ref`/`out` on an indexer parameter list, so the
// indexer's function-typed parameter is plain and therefore call-bounded
// (§28.14) — the producer's +1 is the CALLER's to drop once the accessor has
// returned.
//
// RED on the base (SIGSEGV under ASan), GREEN with the fix — and
// LeakSanitizer must count NOTHING, so the fix may not trade the crash for a
// leak.
BEZEL_TEST(virtual_indexer_empty_setter_body_9152,
           checked_oracle_lambda_index_argument_leaks_nothing) {
    const std::string src =
        "module M; "
        "import System; "
        "public class Fl "
        "  private _v: int := 0; "
        "  public virtual property [q: () -> int]: int "
        "    get => _v; "
        "    set => do end; "
        "  end; "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    using var fl := new Fl(); "
        "    for var j in 0..<200 do "
        "      fl[lambda() => 7] := 5; "
        "    end; "
        "    Console.WriteLine(0); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectCleanChecked(runAotChecked(src), 0, "0\n");
}

// ── Row 4 (GREEN CONTROL — the issue's non-empty-setter twin): the same
// virtual indexer with an ORDINARY setter body. Green on the base and green
// with the fix; it fails if the fix disturbs an ordinary virtual indexer
// setter (a wrong signature, a lost store, a double release).
BEZEL_TEST(virtual_indexer_empty_setter_body_9152,
           non_empty_setter_control_still_stores) {
    const std::string src =
        "module M; "
        "import System; "
        "public class Fl "
        "  private _v: int := 0; "
        "  public virtual property [i: int]: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "  public constructor() end constructor; "
        "end Fl; "
        "public class Program "
        "  public static method Main(): int "
        "    using var fl := new Fl(); "
        "    fl[0] := 5; "
        "    Console.WriteLine(fl[0]); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    expectClean(runAot(src), 0, "5\n");
}

}  // namespace
