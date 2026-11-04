// Codegen — §8.6 / §28.3 / §28.9 / #4841: the §8.6 ARGUMENT BOX minted for a
// value argument bound to an `Object` / interface CONSTRUCTOR parameter is
// released when the constructor returns.
//
// THE DEFECT (MEASURED on this fix's parent, `89e3a353`, with build/bezelc
// under ASan/LSan). A value (or record) argument bound to an `Object` /
// interface parameter is §8.6-boxed at the CALL SITE (`boxForTarget` inside
// `buildOrderedArgs`). The box is a CALLER-side +1 producer whose lifetime
// ends when the constructor returns — §28.3 "Unbound owned temporaries" /
// §28.9 "Runtime-managed types".
//
// #2952 discharges exactly that shape after an ordinary METHOD call, and the
// `new` path did not: `buildOrderedArgs` queues the box for release only when
// `currentCallee_` is set (the escape analysis needs the callee's body), and
// the METHOD call path installs it (LlvmBackend_Calls2.cpp) while the
// CONSTRUCTOR path never did. So the box was minted and never queued.
//
//   MEASURED (ASan/LSan, this fix's parent):
//     one `new C(5)` with `constructor(v: Object)`  →  24 B leaked
//     300 such constructions in a loop              →  7200 B in 300 allocations
//
// The issue's own oracle: "loop-amplify (≥50 iterations) over locals and run
// under `runAotChecked`. A one-shot LSan row can pass vacuously." — hence the
// 300-iteration rows below.
//
// THE FIX (src/codegen/LlvmBackend_Exprs2.cpp — `Emitter::lowerNew`). The
// `new` path now does the two halves the METHOD path already has:
//   * it installs the CONSTRUCTOR on `currentCallee_` around the argument
//     build, so `isCalleeParamNonEscaping` (LlvmBackend_Assign.cpp — which
//     already has its `ConstructorDecl` arm) can walk the constructor's body
//     and decide whether the boxed parameter escapes; and
//   * it opens THIS `new`'s own §8.6 argument-box record (`beginArgBoxCall`)
//     and discharges it (`dischargeArgBoxCall`) after the constructor has
//     returned — the per-CALL record discipline of #7998, so a NESTED `new`'s
//     discharge can never release an OUTER `new`'s still-live box.
//
// ORACLE DISCIPLINE. Rows A–D run under the harness's ASan/LeakSanitizer leg
// (`runAotChecked`), whose leak verdict is a FAIL: the box's release is what
// the rows observe, not a printed value. Each row ALSO asserts the positive —
// the exact stdout and the exact exit code — so a row that merely "ran" cannot
// pass vacuously.
//
// ROW F IS THE CONTROL, and it is deliberately NOT a leak row: a constructor
// that STORES the parameter into a field keeps the box alive past the
// constructor, so the escape analysis declines the release and the box is
// reclaimed by the field's own disposal instead. That row asserts the VALUE
// (the program prints "5"), i.e. that the fix does not free a box the callee
// still owns. Its residual leak (the field-held box) is a DIFFERENT, still
// open defect (#6827's family) and is not this row's subject.
//
// §8.6 "Boxing", §28.3 "Local variables, ownership, and borrows" (the
// "Unbound owned temporaries" rule), §28.9 "Runtime-managed types".

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// (A) THE ISSUE'S HEADLINE — a value argument bound to an `Object`
// constructor parameter, loop-amplified 300×. The box is one 24-byte
// allocation per construction; the row observes the ABSENCE of the leak
// through the sanitizer leg, and asserts the program's own output.
const char* kObjectCtorArg =
    "module M; "
    "public class C "
    "  public readonly property Tag: string; "
    "  public constructor(v: Object) "
    "    Tag := \"IC\"; "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; "
    "    while i < 300 do "
    "      var a := new C(5); "
    "      _ := a.Tag; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// (B) THE NAMED-ARGUMENT SPELLING (§22.5) — `new C(v := 5)` binds the same
// formal by name; the box must be released exactly the same way.
const char* kNamedObjectCtorArg =
    "module M; "
    "public class C "
    "  public readonly property Tag: string; "
    "  public constructor(v: Object) "
    "    Tag := \"IC\"; "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; "
    "    while i < 300 do "
    "      var b := new C(v := 5); "
    "      _ := b.Tag; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// (C) THE INTERFACE-PARAMETER ROW — `IComparable<int>` is an interface
// formal, so the argument is boxed for the interface slot exactly as it is
// for `Object`.
const char* kIComparableCtorArg =
    "module M; "
    "public class C "
    "  public readonly property Tag: string; "
    "  public constructor(v: IComparable<int>) "
    "    Tag := \"IC\"; "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; "
    "    while i < 300 do "
    "      var c := new C(5); "
    "      _ := c.Tag; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// (D) THE `IEquatable<bool>` ROW WITH A DEFAULTED SIBLING — the issue names
// this shape too ("the same holds for `bool` into `IEquatable<bool>`"). The
// defaulted second parameter must not change the first one's box lifetime.
const char* kIEquatableCtorArg =
    "module M; "
    "public class C "
    "  public readonly property Tag: string; "
    "  public constructor(v: IEquatable<bool>, w: int := 3) "
    "    Tag := \"IE\"; "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; "
    "    while i < 300 do "
    "      var d := new C(true); "
    "      _ := d.Tag; "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine(\"done\"); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// (E) THE VALUE ROW — the constructor READS the boxed parameter
// (`v.ToString()`), so the box must still be alive when the constructor runs:
// a release emitted too early would read freed memory (ASan: heap-use-after-
// free) or print garbage. The row asserts the exact rendering "5".
const char* kCtorReadsBoxedArg =
    "module M; "
    "public class C "
    "  public readonly property Tag: string; "
    "  public constructor(v: Object) "
    "    Tag := v.ToString(); "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var e := new C(5); "
    "    Console.WriteLine(e.Tag); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// (F) THE CONTROL — the constructor KEEPS the parameter (stores it into a
// field). The escape analysis must decline the release here, so the box is
// still readable after the constructor returns. NOT a leak row: the field-held
// box's own reclamation is a different (still open) defect.
const char* kCtorKeepsBoxedArg =
    "module M; "
    "public class C "
    "  public data: Object; "
    "  public constructor(v: Object) "
    "    data := v; "
    "  end constructor; "
    "end C; "
    "public class Program "
    "  public static method Main(): int "
    "    var f := new C(5); "
    "    Console.WriteLine(f.data); "
    "    return 0; "
    "  end Main; "
    "end Program;";

}  // namespace

// =====================================================================
// (A) #4841's headline: the §8.6 argument box of a constructor argument
// is released when the constructor returns — observed through the
// ASan/LeakSanitizer leg (24 B per construction on the pre-fix tree).
// §8.6 / §28.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_ctor_object_arg_box_release_4841,
           boxed_object_ctor_argument_is_released_spec_8_6_28_3) {
    JitResult r = runAotChecked(kObjectCtorArg);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "done\n");
}

// =====================================================================
// (B) The named-argument spelling (§22.5) releases the same box.
// §8.6 / §28.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_ctor_object_arg_box_release_4841,
           named_boxed_object_ctor_argument_is_released_spec_8_6_28_3) {
    JitResult r = runAotChecked(kNamedObjectCtorArg);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "done\n");
}

// =====================================================================
// (C) An INTERFACE formal (`IComparable<int>`) is boxed for the
// interface slot and released the same way.
// §8.6 / §28.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_ctor_object_arg_box_release_4841,
           boxed_interface_ctor_argument_is_released_spec_8_6_28_3) {
    JitResult r = runAotChecked(kIComparableCtorArg);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "done\n");
}

// =====================================================================
// (D) The `IEquatable<bool>` formal, with a defaulted sibling parameter.
// §8.6 / §28.3 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_ctor_object_arg_box_release_4841,
           boxed_iequatable_ctor_argument_is_released_spec_8_6_28_3) {
    JitResult r = runAotChecked(kIEquatableCtorArg);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "done\n");
}

// =====================================================================
// (E) The box is ALIVE while the constructor reads it: the rendering is
// the boxed value's own ("5"), not a freed box's garbage.
// §8.6 / §8.5 / §28.9.
// =====================================================================
BEZEL_TEST(codegen_ctor_object_arg_box_release_4841,
           boxed_ctor_argument_is_live_while_the_constructor_reads_it) {
    JitResult r = runAotChecked(kCtorReadsBoxedArg);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}

// =====================================================================
// (F) THE CONTROL: a constructor that KEEPS the parameter stores it into
// a field, so the escape analysis declines the release and the box is
// still readable after the constructor returns. The value is the oracle
// here — the box's own reclamation is a separate, still-open defect.
// §8.6 / §28.3.
// =====================================================================
BEZEL_TEST(codegen_ctor_object_arg_box_release_4841,
           boxed_ctor_argument_kept_by_the_constructor_stays_readable) {
    JitResult r = runAot(kCtorKeepsBoxedArg);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n");
}
