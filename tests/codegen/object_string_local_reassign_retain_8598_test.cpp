// Codegen — an `Object`/interface-typed LOCAL REASSIGNED from a borrowed
// `string` took no reference of its own: the source's release freed the
// string under the slot (and under any field the slot was stored into).
// §28.9 (Runtime-managed types), §8.6 (a reference type is not boxed),
// v0.357.
//
// THE DEFECT (issue #8598). The `Object := string` binding path that
// retains is the LocalDecl one: #2900 / #8307 / #8514 make
// `var o: Object := s;` a string-aliased slot that RETAINS at bind (and
// releases at scope end when the alias never escapes), so the source `s`
// keeps its own reference and its own scope-end release. The REASSIGNMENT
// `o := s;` had no such path: `Emitter::isObjectStringSource` classified
// only a bare `string` LOCAL identifier as a string source, so a `string`
// PARAMETER (whose reference the CALLER releases after the call) and a
// `string` FIELD read (whose reference the field releases on the next
// store) were stored raw — with no reference of the slot's own.
//
// MEASURED on the pre-fix build (r30-c1 52f9c7247), `--sanitize=address`,
// 200 iterations, `ASAN_OPTIONS=detect_leaks=1`: heap-use-after-free for
// every fix row below — the parameter repro, the field-read repro, and the
// class-instance-initializer variant.
//
// THE FIX (§28.9, v0.357). §28.9 leaves a string's lifetime to the
// runtime, so a slot that can outlive the source must hold a reference:
// the reassignment RETAINS the string into the `Object` slot, exactly as
// the bind-time path does. The reference is dropped again where the slot's
// own scope-end release is armed (the escape walk proved the alias never
// escapes); an alias that ESCAPES into a field keeps its reference — the
// documented residual LEAK of a string behind an `Object` erasure (#5059,
// #8459), never a dangling read. That is the same trade the sibling BIND
// shape (`var o: Object := x;`, #8514) already makes, so the two spellings
// now agree.
//
// A slot whose own initializer is NOT a string (the class-instance and the
// no-initializer variants) is never admitted as a string-aliased slot, so no
// scope-end release is armed for it and the reference the slot took is not
// dropped again: that string leaks. The leak is the same SIZE as the leak
// the pre-fix build already reported for the other shapes (7290 bytes / 400
// allocations for the 200-iteration programs), so no new leak is added — the
// fix trades a dangling read for a bounded leak, the same trade the escaping
// alias makes.
//
// The heap-store rows run on `runAotCheckedLenient` (ASan armed, LSan off):
// the field's documented +1 leak is not this row's subject. The
// leak-checked rows run on `runAotChecked`. Every row asserts
// `!checkedToolchainSkipped(r)` before any guard that could absorb the
// skip.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The defect is a REFCOUNT defect: one iteration can hide it, 200 cannot.
constexpr int kIterations = 200;

// Sum over i in 0..199 of ("s" + i).Length: 10 * 2 + 90 * 3 + 100 * 4.
const char* kLengthTotal = "690\n";

const char* kDecls =
    "public class Cell "
    "  public V: int := 0; "
    "end Cell; "
    "public class Holder "
    "  public O: Object := \"\"; "
    "  public S: string := \"\"; "
    "  public owned C: Cell := new Cell(); "
    // The issue's own shape: an `Object` local REASSIGNED from a `string`
    // PARAMETER, then stored into the field.
    "  public method ReassignLitKeep(x: string) "
    "    var o: Object := \"\"; "
    "    o := x; "
    "    this.O := o; "
    "  end ReassignLitKeep; "
    // The same over a `string` FIELD read.
    "  public method ReassignFieldKeep() "
    "    var o: Object := \"\"; "
    "    o := this.S; "
    "    this.O := o; "
    "  end ReassignFieldKeep; "
    // The read-only twin: the alias is only READ inside the block.
    "  public method ReassignLitRead(x: string): int "
    "    var o: Object := \"\"; "
    "    o := x; "
    "    return o.ToString().Length; "
    "  end ReassignLitRead; "
    // The alias is REASSIGNED to a class instance: the string release must
    // be skipped (the slot no longer holds a string).
    "  public method ReassignParamToCell(x: string, c: Cell): int "
    "    var o: Object := \"\"; "
    "    o := x; "
    "    var n := o.ToString().Length; "
    "    o := c; "
    "    return n; "
    "  end ReassignParamToCell; "
    // The slot's own initializer is a CLASS INSTANCE, not a string: the
    // slot still owes the string it is later given a reference of its own.
    "  public method ReassignClassInitKeep(x: string, c: Cell) "
    "    var o: Object := c; "
    "    o := x; "
    "    this.O := o; "
    "  end ReassignClassInitKeep; "
    "end Holder; "
    "public class Make "
    "  public static method Str(n: int): string => \"s\" + n.ToString(); "
    "end Make; ";

// `body` runs once per iteration with `h` (a fresh Holder), `i` and `total`
// in scope; the program prints `total`.
std::string program(const std::string& body) {
    return std::string("module M; ") + kDecls +
           "public class Program "
           "  public static method Main(): int "
           "    var i := 0; var total := 0; "
           "    while i < " + std::to_string(kIterations) + " do "
           "      var h := new Holder(); "
           + body +
           "      i := i + 1; "
           "    end; "
           "    Console.WriteLine(total); "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// A heap string is stored into the field by the body's `do ... end` block,
// whose end releases the string's owner; the field is read back afterwards
// (after a fresh allocation that would reuse a freed block).
std::string storeThenRead(const std::string& store) {
    return program("      do " + store + " end; "
                   "      var junk := Make.Str(99999); "
                   "      total := total + h.O.ToString().Length + "
                   "junk.Length - junk.Length; ");
}

// The alias is only READ inside the block.
std::string readOnly(const std::string& block) {
    return program("      do " + block + " end; ");
}

// ASan armed, LSan off: ran, exit 0, exact stdout. A use-after-free, double
// free or over-release fails the row; the field's documented +1 leak does not.
void expectNoUseAfterFree(const std::string& src, const std::string& expected) {
    JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// ASan AND LSan armed: ran, exit 0, exact stdout, nothing leaked.
void expectCleanRun(const std::string& src, const std::string& expected) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN SHAPES AND THEIR VARIANTS (#8598).
// ===========================================================================

// The issue's repro: an `Object` local REASSIGNED from a `string` PARAMETER,
// stored into the field, read back after the caller released its argument.
// MEASURED heap-use-after-free on the pre-fix build.
BEZEL_TEST(object_string_local_reassign_retain_8598,
           issue_reassign_param_object_local_stored_spec_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); h.ReassignLitKeep(x);"),
        kLengthTotal);
}

// The other source kind the issue names: a `string` FIELD read, whose
// reference the field releases on the next store. MEASURED
// heap-use-after-free on the pre-fix build.
BEZEL_TEST(object_string_local_reassign_retain_8598,
           issue_reassign_field_read_object_local_stored_spec_28_9) {
    expectCleanRun(
        program("      var k := new Holder(); "
                "      do k.S := Make.Str(i); "
                "         var o: Object := \"\"; o := k.S; k.S := \"zz\"; "
                "         total := total + o.ToString().Length; "
                "      end; "),
        kLengthTotal);
}

// The slot's own initializer is a CLASS INSTANCE, not a string: the slot is
// never a string-aliased slot (the collector admits a slot whose initializer
// IS a string), so nothing about it is release-tracked — but the REASSIGNMENT
// still stores a borrowed string into it, and the reference the slot must own
// belongs to the SLOT, not to the initializer. MEASURED heap-use-after-free on
// the pre-fix build.
BEZEL_TEST(object_string_local_reassign_retain_8598,
           issue_reassign_class_init_object_local_stored_spec_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); "
                      "h.ReassignClassInitKeep(x, h.C);"),
        kLengthTotal);
}

// ===========================================================================
// CONTROLS — leak-checked. A read-only alias keeps both the source's and
// the alias's release.
// ===========================================================================

// BOUNDARY CONTROL (base-green, kept: an rc leak check a corpus case cannot
// express). Goes RED under the wrong fix `retain at the reassignment and
// never release the slot's own reference` (a read-only alias would then
// leak one string per iteration).
BEZEL_TEST(object_string_local_reassign_retain_8598,
           control_reassign_param_object_local_read_only_spec_28_9) {
    expectCleanRun(
        readOnly("var x := Make.Str(i); "
                 "total := total + h.ReassignLitRead(x);"),
        kLengthTotal);
}

// BOUNDARY CONTROL (base-green, kept: the state-bit check a corpus case
// cannot express). Goes RED under the wrong fix `release the displaced
// value at every reassignment, without the state bit's gate` (a class
// instance would be released as a string).
BEZEL_TEST(object_string_local_reassign_retain_8598,
           control_reassign_to_class_instance_skips_string_release_spec_28_9) {
    expectCleanRun(
        program("      do var x := Make.Str(i); "
                "         total := total + h.ReassignParamToCell(x, h.C); "
                "      end; "),
        kLengthTotal);
}
