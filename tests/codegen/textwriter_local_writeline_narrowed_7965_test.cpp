// Regression for #7965 -- `t.WriteLine(p)` on an ANNOTATED `TextWriter` local
// with a NARROWED nullable RECORD argument did not compile at all:
//
//   record P(X: int, Y: int);
//   class C
//     public static method Show(p: P?)
//       var t: TextWriter := Console.Out;
//       if p <> null then t.WriteLine(p); end;
//     end Show;
//   ...
//
// MEASURED on b0325c3c (the staging tip this lane started from):
//
//   Both operands to ICmp instruction are not of the same type!
//     %has.val2 = icmp ne i32 %has.val81, i8 0
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// THE DEFECT. stdlib §7.4 declares `TextWriter.WriteLine(value: Object)`, and
// §7.4 / §22.6 say that "a read of a binding §7.4 has narrowed non-null is of
// its non-nullable type" -- so the read of `p` inside the `if p <> null` guard
// is a `P`, not a `P?`. Sema PUBLISHES that narrowed read (#7621) for the
// `TextWriter` facet (its §7 nullability gate is `checkBuiltinNullableArgs`,
// which publishes), and codegen's #6674 unwrap then extracts the record from
// the `{i8 hasValue; P}` carrier BEFORE the console lowering runs.
//
// The console lowering (`tryLowerConsoleCall`) still expected the CARRIER: it
// read element 0 of the argument as the has-value tag, and the argument was
// the bare record struct, so element 0 was the record's FIRST FIELD (an i32):
//
//   %has.val81 = extractvalue %record.P %nv.val, 0   ; the X field!
//   %has.val2  = icmp ne i32 %has.val81, i8 0        ; width mismatch
//
// => the IR verifier rejected a LEGAL program. The `Console.WriteLine(p)` /
// `Console.Out.WriteLine(p)` spellings were UNAFFECTED: their §7 gate REPORTS
// but does not publish, so the carrier reaches the console lowering and the
// has-value test reads the real tag.
//
// The same shape over a nullable CHOICE was worse than a verifier failure:
// `emitChoiceToString` read the payload's case slot out of the payload and the
// compiler died with SIGSEGV (measured: `t.WriteLine(c)` over a narrowed
// `VC?` -- no diagnostic, no exit status, a core dump).
//
// WHAT IS PINNED HERE (the #7965 shapes, and the controls that must not move):
//   * a narrowed nullable RECORD through the `TextWriter` local  (the repro)
//   * a narrowed nullable CHOICE through the `TextWriter` local (the crash)
//   * the `Console.WriteLine` / `Console.Out.WriteLine` spellings of the same
//     write -- they must keep printing exactly what they printed before
//   * the un-narrowed spelling -- still rejected by §7's nullability gate
//   * a 100x loop -- the render is a fresh +1 the console owns and discharges
//     (stdlib §2.1 / §28.9), so the ASan leg must stay clean

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The issue's own repro shape: a `P?` parameter, narrowed by a null test,
// written through a `TextWriter` local bound to `Console.Out`.
const char* kPre =
    "module M; "
    "record P(X: int, Y: int); "
    "choice VC A(x: int), B; end VC; "
    "enum E One, Two; end E; "
    "class C "
    "  public static method Show(p: P?, c: VC?, e: E?) "
    "    var t: TextWriter := Console.Out; ";

const char* kPost =
    " end Show; end C; "
    "class D public static method Main(): int "
    "  C.Show(new P(1, 2), VC.A(3), E.One); return 0; "
    "end Main; end D;";

// The program text of one `Show` body.
std::string program(const char* body) {
    return std::string(kPre) + body + kPost;
}

}  // namespace

// §7.4 / §22.6 / stdlib §7.4 / #7965: the narrowed READ of `p` is of its
// non-nullable type `P`, so the console renders the RECORD -- it did not even
// compile before the fix (IR verification failure).
BEZEL_TEST(textwriter_local_writeline_narrowed_7965, narrowed_record_writeline) {
    auto r = runAot(program("if p <> null then t.WriteLine(p); end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("P(X: 1, Y: 2)\n"));
}

// The same shape over a nullable CHOICE: the compiler used to die with SIGSEGV
// (a core dump, no diagnostic) instead of compiling.
BEZEL_TEST(textwriter_local_writeline_narrowed_7965, narrowed_choice_writeline) {
    auto r = runAot(program("if c <> null then t.WriteLine(c); end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("VC.A(x: 3)\n"));
}

// The other nullable VALUE family through the same facet: an enum's payload is
// an integer and a record's is a struct, so only the struct one broke -- this
// pins that the enum keeps rendering §8.5's type name and that the fix did
// not change it.
BEZEL_TEST(textwriter_local_writeline_narrowed_7965, narrowed_enum_writeline) {
    auto r = runAot(program("if e <> null then t.WriteLine(e); end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("M.E\n"));
}

// THE #5240 HAZARD SHAPE, on the PAYLOAD path. A `record R(b: byte, n: int)`
// lowers to `{i8, i32}` — the very shape of a nullable-value CARRIER — so the
// payload of a nullable `R?` is `{i8, i32}` too, and the payload arm must not
// mistake it for a carrier (nor render the record's SECOND field alone, which
// is what the #5240 cut's structural detection did: `Console.WriteLine(q)`
// printed `99` instead of `R(b: 7, n: 99)`).
BEZEL_TEST(textwriter_local_writeline_narrowed_7965, narrowed_byte_int_record_writeline) {
    auto r = runAot(
        "module M; "
        "record R(b: byte, n: int); "
        "class C "
        "  public static method Show(r: R?) "
        "    var t: TextWriter := Console.Out; "
        "    if r <> null then t.WriteLine(r); end; "
        "  end Show; "
        "  public static method Main(): int "
        "    C.Show(new R(7, 99)); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("R(b: 7, n: 99)\n"));
}

// THE CONTROLS: the `Console` spellings of the same write keep printing
// exactly what they printed before the fix (their §7 gate REPORTS but does not
// publish, so the carrier reaches the console lowering). A behaviour flip here
// would be a regression, not a fix.
BEZEL_TEST(textwriter_local_writeline_narrowed_7965, console_facets_unchanged) {
    auto r = runAot(
        "module M; "
        "record P(X: int, Y: int); "
        "class C "
        "  public static method Show(p: P?) "
        "    if p <> null then Console.WriteLine(p); end; "
        "    if p <> null then Console.Out.WriteLine(p); end; "
        "  end Show; "
        "  public static method Main(): int "
        "    C.Show(new P(1, 2)); return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("P(X: 1, Y: 2)\nP(X: 1, Y: 2)\n"));
}

// The UN-narrowed spelling is still rejected by §7's nullability gate: the
// `value: Object` parameter of stdlib §7.4's `WriteLine` is non-nullable, so
// a `P?` that §7.4 has not narrowed may not reach it.
BEZEL_TEST(textwriter_local_writeline_narrowed_7965, un_narrowed_argument_rejected) {
    auto r = runAot(program("t.WriteLine(p); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "nullable but the target type is non-nullable"));
}

// The render is a fresh +1 the console owns and discharges (stdlib §2.1 /
// §28.9): 100 writes of a narrowed record leak nothing. This row is on
// `runAotChecked` (the ASan/LSan memory net), not on `runAot` -- `runAot`
// does not leak-check (tests/README.md).
BEZEL_TEST(textwriter_local_writeline_narrowed_7965, narrowed_record_loop_no_leak) {
    auto r = runAotChecked(program(
        "for var i in 0..<100 do "
        "  if p <> null then t.WriteLine(p); end; "
        "end for; "));
    BEZEL_EXPECT_TRUE(r.ran);
}
