// Codegen — an `Object`/interface-typed LOCAL bound to a string THROUGH `!`
// took no reference while the string's owner still released it: a
// use-after-free once the alias was stored anywhere that outlives the owner.
// §17.8 (`x!` "preserves ownership"), §17.4 ("For `as`, the result is
// additionally a *borrow* of `x`"), §28.9 (Runtime-managed types), §8.6 (a
// reference type is not boxed), v0.357.
//
// THE DEFECT (issue #8514, a #8307 residual). #2900 / #8307 make
// `var o: Object := x;` and `var o := x as Object;` a string-aliased slot
// that RETAINS at bind (and releases at scope end when the alias never
// escapes), so the source `x` keeps its own release. The same slot bound
// through the null-forgiving `!` was never admitted — yet the escape walk's
// `!` arm (#2201, written for the `string`-typed `var t := s!`, which IS
// retained at bind) read the source as merely tested. So `x`'s scope-end
// release fired with the un-retained alias still naming the string.
//
// MEASURED on the pre-fix build (r30-c1 0cb32bb14), `--sanitize=address`,
// each of the three issue shapes below: heap-use-after-free.
//
// THE FIX. The `!` spellings join the string-aliased `Object` locals when
// some layer under the run of `!` / pointer-preserving `as` is STATICALLY
// `string` (the type that makes the bind-time retain sound); the walk's `!`
// arm reads the source as safe only for a local that does take a reference,
// and treats every other `Object`/interface-typed `!` bind as the escape it
// is. A stored alias therefore keeps its reference — the documented residual
// LEAK of a string behind an `Object` erasure (#5059, #8459), never a
// dangling read — and a read-only alias stays leak-free (the CONTROLS).
//
// THE SOURCE KIND. The slot owes its reference whatever names the string: a
// `string?` PARAMETER bound through `!` (the caller releases its argument
// after the call) dangled the same way, and so did the un-wrapped twin over a
// `string` parameter or a `string` field read (released when the field is
// stored over). Any statically-`string`, non-producer initializer is now
// admitted — the rule the `string`-typed borrow local (#429) already follows.
// MEASURED heap-use-after-free on the pre-fix build for each "SOURCE KIND"
// heap-store row below.
//
// The heap-store rows run on `runAotCheckedLenient` (ASan armed, LSan off);
// the controls on `runAotChecked` (leak-checked). Every row asserts
// `!checkedToolchainSkipped(r)` before any guard that could absorb the skip.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The defect is a REFCOUNT defect: one iteration can hide it, 100 cannot.
constexpr int kIterations = 100;

// Sum over i in 0..99 of ("s" + i).Length: 10 * 2 + 90 * 3.
const char* kLengthTotal = "290\n";

const char* kDecls =
    "public class Holder "
    "  public O: Object := \"\"; "
    "  public method Put(o: Object) "
    "    this.O := o; "
    "  end Put; "
    "  public method Peek(o: Object): int => o.ToString().Length; "
    "  public S: string := \"\"; "
    "  public method PutB(x: string?) "
    "    var o: Object := x!; this.O := o; "
    "  end PutB; "
    "  public method PutS(x: string) "
    "    var o: Object := x; this.O := o; "
    "  end PutS; "
    "  public method KeepF() "
    "    var o: Object := this.S; this.O := o; "
    "  end KeepF; "
    "  public method PeekB(x: string?): int "
    "    var o: Object := x!; return o.ToString().Length; "
    "  end PeekB; "
    "  public method PeekS(x: string): int "
    "    var o: Object := x; return o.ToString().Length; "
    "  end PeekS; "
    "  public method PeekF(): int "
    "    var o: Object := this.S; return o.ToString().Length; "
    "  end PeekF; "
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
// THE ISSUE'S OWN SHAPES (#8514).
// ===========================================================================

// y07 / f01: a `string?` local bound through `!` into an `Object` local, then
// stored (§17.8: `x!` preserves ownership — the alias holds `x`'s pointer).
BEZEL_TEST(object_string_bang_alias_8514,
           issue_y07_nullable_string_bang_object_local_stored_spec_17_8_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var x: string? := Make.Str(i); var o: Object := x!; "
                      "h.O := o;"),
        kLengthTotal);
}

// y01 / f02: `(x as Object)!` bound to an inferred `Object` local, then stored.
BEZEL_TEST(object_string_bang_alias_8514,
           issue_y01_as_object_bang_alias_stored_spec_17_8_17_4_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); var o := (x as Object)!; "
                      "h.O := o;"),
        kLengthTotal);
}

// y02 / f03: an `Object` holder cast back with `(o as string)!` into a second
// `Object` local, then stored.
BEZEL_TEST(object_string_bang_alias_8514,
           issue_y02_object_holder_as_string_bang_stored_spec_17_8_17_4_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var o: Object := Make.Str(i); "
                      "var p: Object := (o as string)!; h.O := p;"),
        kLengthTotal);
}

// ===========================================================================
// NEIGHBOURS of the same bind.
// ===========================================================================

// The alias handed to an `Object` formal the callee stores.
BEZEL_TEST(object_string_bang_alias_8514,
           bang_object_local_passed_to_storing_formal_spec_17_8_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var x: string? := Make.Str(i); var o: Object := x!; "
                      "h.Put(o);"),
        kLengthTotal);
}

// The alias copied onward before the store.
BEZEL_TEST(object_string_bang_alias_8514,
           bang_object_local_copied_then_stored_spec_17_8_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var x: string? := Make.Str(i); var o: Object := x!; "
                      "var p := o; h.O := p;"),
        kLengthTotal);
}

// The alias stored twice.
BEZEL_TEST(object_string_bang_alias_8514,
           bang_object_local_stored_twice_spec_17_8_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var x: string? := Make.Str(i); var o: Object := x!; "
                      "h.O := o; h.O := o;"),
        kLengthTotal);
}

// The alias declared in a nested block / a branch.
BEZEL_TEST(object_string_bang_alias_8514,
           bang_object_local_in_nested_block_stored_spec_17_8_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var x: string? := Make.Str(i); "
                      "do var o: Object := x!; h.O := o; end;"),
        kLengthTotal);
}
BEZEL_TEST(object_string_bang_alias_8514,
           bang_object_local_in_if_branch_stored_spec_17_8_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var x: string? := Make.Str(i); "
                      "if i >= 0 then var o: Object := x!; h.O := o; end;"),
        kLengthTotal);
}

// An interface-typed alias (`string` conforms to `IEquatable<string>`).
BEZEL_TEST(object_string_bang_alias_8514,
           as_interface_bang_alias_stored_spec_17_8_17_4_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); "
                      "var e := (x as IEquatable<string>)!; h.O := e;"),
        kLengthTotal);
}

// The SOURCE is reassigned while the alias is alive: the alias must hold a
// reference of its own (the reassignment releases the displaced string).
BEZEL_TEST(object_string_bang_alias_8514,
           bang_object_local_survives_source_reassignment_spec_17_8_28_9) {
    expectNoUseAfterFree(
        readOnly("var x: string? := Make.Str(i); var o: Object := x!; "
                 "x := Make.Str(i + 1000); "
                 "var junk := Make.Str(99999); "
                 "total := total + o.ToString().Length + junk.Length - "
                 "junk.Length;"),
        kLengthTotal);
}

// No STATICALLY-string layer (`o!` over an `Object?`): the alias cannot be
// retained as a string, so the bind is an escape of the holder.
BEZEL_TEST(object_string_bang_alias_8514,
           nullable_object_holder_bang_object_local_stored_spec_17_8_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var o: Object? := Make.Str(i); var p: Object := o!; "
                      "h.O := p;"),
        kLengthTotal);
}

// ===========================================================================
// CONTROLS — leak-checked. A read-only `!` alias keeps both the source's and
// the alias's release.
// ===========================================================================

BEZEL_TEST(object_string_bang_alias_8514,
           control_nullable_string_bang_object_local_read_only_spec_17_8_28_9) {
    expectCleanRun(
        readOnly("var x: string? := Make.Str(i); var o: Object := x!; "
                 "total := total + o.ToString().Length;"),
        kLengthTotal);
}
BEZEL_TEST(object_string_bang_alias_8514,
           control_as_object_bang_alias_read_only_spec_17_8_17_4_28_9) {
    expectCleanRun(
        readOnly("var x := Make.Str(i); var o := (x as Object)!; "
                 "total := total + o.ToString().Length;"),
        kLengthTotal);
}
BEZEL_TEST(object_string_bang_alias_8514,
           control_object_holder_as_string_bang_read_only_spec_17_8_17_4_28_9) {
    expectCleanRun(
        readOnly("var o: Object := Make.Str(i); "
                 "var p: Object := (o as string)!; "
                 "total := total + p.ToString().Length;"),
        kLengthTotal);
}
BEZEL_TEST(object_string_bang_alias_8514,
           control_bang_alias_passed_to_reading_formal_spec_17_8_28_9) {
    expectCleanRun(
        readOnly("var x := Make.Str(i); var o: Object := (x as Object)!; "
                 "total := total + h.Peek(o);"),
        kLengthTotal);
}
BEZEL_TEST(object_string_bang_alias_8514,
           control_const_bang_object_alias_read_only_spec_17_8_21_1_28_9) {
    expectCleanRun(
        readOnly("var x: string? := Make.Str(i); const o: Object := x!; "
                 "total := total + o.ToString().Length;"),
        kLengthTotal);
}
// The `string`-typed `!` bind #2201 was written for is unchanged.
BEZEL_TEST(object_string_bang_alias_8514,
           control_string_bang_alias_read_only_spec_17_8_28_9) {
    expectCleanRun(
        readOnly("var x: string? := Make.Str(i); var t := x!; "
                 "total := total + t.Length;"),
        kLengthTotal);
}

// ===========================================================================
// THE SOURCE KIND — a parameter or a field read, not a `var` local.
// ===========================================================================

// A `string?` PARAMETER bound through `!` inside the callee, then stored: the
// caller's release of its argument (a named local's scope end; an unbound
// producer's post-call release) must not free the stored string.
BEZEL_TEST(object_string_bang_alias_8514,
           source_kind_nullable_param_bang_object_local_stored_spec_17_8_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var x: string? := Make.Str(i); h.PutB(x);"),
        kLengthTotal);
}
BEZEL_TEST(object_string_bang_alias_8514,
           source_kind_producer_arg_bang_object_local_stored_spec_17_8_28_9) {
    expectNoUseAfterFree(storeThenRead("h.PutB(Make.Str(i));"), kLengthTotal);
}

// The un-wrapped twin over a `string` parameter.
BEZEL_TEST(object_string_bang_alias_8514,
           source_kind_string_param_object_local_stored_spec_28_9_8_6) {
    expectNoUseAfterFree(storeThenRead("var x := Make.Str(i); h.PutS(x);"),
                         kLengthTotal);
}
BEZEL_TEST(object_string_bang_alias_8514,
           source_kind_producer_arg_object_local_stored_spec_28_9_8_6) {
    expectNoUseAfterFree(storeThenRead("h.PutS(Make.Str(i));"), kLengthTotal);
}

// A `string` FIELD read: the alias must survive the field being stored over
// (which releases the displaced string).
BEZEL_TEST(object_string_bang_alias_8514,
           source_kind_field_read_object_local_stored_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("var k := new Holder(); k.S := Make.Str(i); "
                      "do var o: Object := k.S; h.O := o; end; k.S := \"zz\";"),
        kLengthTotal);
}
BEZEL_TEST(object_string_bang_alias_8514,
           source_kind_this_field_read_object_local_stored_spec_28_9_8_6) {
    expectNoUseAfterFree(
        storeThenRead("h.S := Make.Str(i); h.KeepF(); h.S := \"zz\";"),
        kLengthTotal);
}

// Leak-checked: the alias of a field read that is stored over while the alias
// is alive, then only READ. The alias's own reference is released at its
// scope end.
BEZEL_TEST(object_string_bang_alias_8514,
           source_kind_field_read_alias_read_after_overwrite_clean_spec_28_9) {
    expectCleanRun(
        readOnly("var k := new Holder(); k.S := Make.Str(i); "
                 "var o: Object := k.S; k.S := \"zz\"; "
                 "total := total + o.ToString().Length;"),
        kLengthTotal);
}

// Leak-checked controls: a read-only alias of a parameter / a field read
// retains at bind and releases at scope end.
BEZEL_TEST(object_string_bang_alias_8514,
           control_source_kind_param_bang_alias_read_only_spec_17_8_28_9) {
    expectCleanRun(
        readOnly("var x: string? := Make.Str(i); total := total + h.PeekB(x);"),
        kLengthTotal);
}
BEZEL_TEST(object_string_bang_alias_8514,
           control_source_kind_param_alias_read_only_spec_28_9) {
    expectCleanRun(
        readOnly("var x := Make.Str(i); total := total + h.PeekS(x);"),
        kLengthTotal);
}
BEZEL_TEST(object_string_bang_alias_8514,
           control_source_kind_producer_arg_alias_read_only_spec_28_9) {
    expectCleanRun(readOnly("total := total + h.PeekS(Make.Str(i));"),
                   kLengthTotal);
}
BEZEL_TEST(object_string_bang_alias_8514,
           control_source_kind_field_read_alias_read_only_spec_28_9) {
    expectCleanRun(
        readOnly("h.S := Make.Str(i); total := total + h.PeekF();"),
        kLengthTotal);
}
