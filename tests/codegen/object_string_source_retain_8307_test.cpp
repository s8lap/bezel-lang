// Codegen — a `string` reaching an `Object`-typed field through an
// `Object`-typed SOURCE (an `as Object` alias local, an `Object` local, or an
// `Object`/interface PARAMETER) was stored without a retain while the string's
// owner still released it: use-after-free. §28.9 (Runtime-managed types),
// §17.4 ("For `as`, the result is additionally a *borrow* of `x`"), §8.6,
// §28.3 (plain parameters are borrows), v0.357.
//
// THE DEFECT (issue #8307, the #8214 residual). #8214 made a store into an
// `Object`/interface class field RETAIN the value when its source is
// statically a `string` (`h.O := x`, `h.O := x as Object`). A source whose
// static type is `Object` is stored raw — it may as well hold a class
// instance or a box, so retaining it as a string would be unsound. Three
// owners of the string still released it as if the store had retained:
//
//   * the string-escape walk read `x as Object` as a pure TYPE TEST, so
//     `var o := x as Object; h.O := o;` left `x`'s scope-end release armed
//     while the field held `x`'s own pointer;
//   * the walk's #8214 store arm read the RHS of ANY store into such a field
//     as safe, so `var o: Object := Make.Str(i); h.O := o;` released `o`;
//   * the #1430 call-bounded-borrow relaxation (a named local passed to a
//     plain parameter) and the #429 post-call release of an unbound +1
//     producer argument both assume "a field store in the callee retains",
//     which is false for an `Object`/interface formal:
//     `Put(o: Object) this.O := o;` called as `h.Put(x)`,
//     `h.Put(Make.Str(i))` or `new Holder2(Make.Str(i))` freed the string
//     under the field.
//
// MEASURED on the pre-fix build (r30-c1 e49c32aca), every heap-store row
// below: plain build `free(): invalid pointer` (rc 134); `--sanitize=address`
// heap-use-after-free.
//
// THE FIX. `as` (alias-producing) now takes its parent's position in the
// walk; the #8214 store arm fires only for a statically-string source; an
// `Object`/interface formal is a borrow — for the named-local walk and for
// the post-call producer release alike — only when the callee's #2952
// summary (`isCalleeParamNonEscaping`) proves the formal does not outlive the
// call. Everything else keeps the string's +1: the field's reference LEAKS
// (no sound release of a string behind an `Object` erasure exists — #5059),
// which is the pre-#8214 behaviour of every such slot, never a use-after-
// free. `var o := x as Object;` joins #2900's string-aliased Object locals
// (retained at bind, released at scope end when non-escaping), so a
// read-only alias stays leak-free.
//
// `as string` (the cast BACK from an `Object`-typed holder) is an alias too,
// and takes its parent's position for the same reason: its result reaches an
// `Object`-typed consumer raw. MEASURED on the pre-fix build:
// `h.Put((o as string)!)`, `return (o as string)!` from an `Object`-returning
// method, `p := (o as string)!; h.O := p;` and `h.O := ((o as string) as
// Object)!` are each a heap-use-after-free (the AS-STRING rows below). What a
// `string`-typed consumer does with a borrowed string is different: it takes
// its own reference (a bind of a string local retains, a store into a
// string-typed target retains, a callee's store of a `string` formal
// retains), so there the cast stays a READ of its operand and the holder is
// still released at scope end (the AS-STRING CONTROLS below, leak-checked;
// the first of them is the program of
// `codegen_string_optional_hole_producer_5290.cast_as_string_spec_18_5_28_9`).
//
// So the heap-store rows run on `runAotCheckedLenient` (ASan armed, LSan
// off: the field's +1 is the documented residual leak); the controls — a
// callee that only READS its `Object` formal, a read-only alias, a class
// instance through the same formal — stay on `runAotChecked` and are
// leak-checked. Every row asserts `!checkedToolchainSkipped(r)` before any
// guard that could absorb the skip.

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

// The issue's Holder, with the `Put(o: Object)` store of q15 and the
// neighbouring formal shapes: a read-only `Peek`, a transitive `PutVia`, an
// interface formal `PutI`, a dispatched `PutV`; plus the constructor twin.
const char* kDecls =
    "public class Holder "
    "  public O: Object := \"\"; "
    "  public method Put(o: Object) "
    "    this.O := o; "
    "  end Put; "
    "  public method Peek(o: Object): int => o.ToString().Length; "
    "  public method PutVia(o: Object) "
    "    this.Put(o); "
    "  end PutVia; "
    "  public method PutI(e: IEquatable<string>) "
    "    this.O := e; "
    "  end PutI; "
    "  public virtual method PutV(o: Object) "
    "    this.O := o; "
    "  end PutV; "
    "end Holder; "
    "public class Holder2 "
    "  public O: Object := \"\"; "
    "  public constructor(o: Object) "
    "    this.O := o; "
    "  end constructor; "
    "end Holder2; "
    "public class Cell "
    "  public V: int := 0; "
    "end Cell; "
    // The `as string` rows' string-typed consumers, and an `Object`-returning
    // method whose result is a cast of its own local.
    "public class Sink "
    "  public F: string? := null; "
    "  public S: string := \"\"; "
    "  public method Put(x: string?) "
    "    this.F := x; "
    "  end Put; "
    "  public method PeekS(s: string): int => s.Length; "
    "end Sink; "
    "public class Conv "
    "  public static method AsObject(n: int): Object "
    "    var o: Object := \"s\" + n.ToString(); "
    "    return (o as string)!; "
    "  end AsObject; "
    "end Conv; "
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
// THE ISSUE'S OWN SHAPES (#8307's title: `var o := x as Object; h.O := o;`
// and `Put(o: Object)`).
// ===========================================================================

// q14: an `as Object` alias local stored into the field (§17.4: the result of
// `as` is a borrow "with the same source as `x`", so the field holds `x`'s own
// pointer).
BEZEL_TEST(object_string_source_retain_8307,
           issue_q14_as_object_alias_local_stored_spec_28_9_17_4) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); var o := x as Object; h.O := o;"),
        kLengthTotal);
}

// q15: `x as Object` passed to an `Object` formal the callee stores.
BEZEL_TEST(object_string_source_retain_8307,
           issue_q15_as_object_passed_to_storing_formal_spec_28_9_28_3) {
    expectNoUseAfterFree(storeThenRead("var x := Make.Str(i); h.Put(x as Object);"),
                         kLengthTotal);
}

// An `Object`-annotated local initialised from a +1 producer, then stored.
BEZEL_TEST(object_string_source_retain_8307,
           object_local_producer_stored_spec_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var o: Object := Make.Str(i); h.O := o;"), kLengthTotal);
}

// The alias stored twice (an overwrite of the same value).
BEZEL_TEST(object_string_source_retain_8307,
           as_object_alias_stored_twice_spec_28_9_17_4) {
    expectNoUseAfterFree(
        storeThenRead(
            "var x := Make.Str(i); var o := x as Object; h.O := o; h.O := o;"),
        kLengthTotal);
}

// A `string` local passed straight to the storing `Object` formal (the #1430
// relaxation's false "the callee's store retains" premise).
BEZEL_TEST(object_string_source_retain_8307,
           string_local_passed_to_storing_object_formal_spec_28_9_28_3) {
    expectNoUseAfterFree(storeThenRead("var x := Make.Str(i); h.Put(x);"),
                         kLengthTotal);
}

// An unbound +1 producer passed to the storing `Object` formal (the #429
// post-call release's twin of the same premise).
BEZEL_TEST(object_string_source_retain_8307,
           producer_passed_to_storing_object_formal_spec_28_9_28_3) {
    expectNoUseAfterFree(storeThenRead("h.Put(Make.Str(i));"), kLengthTotal);
}

// The `as` alias local passed on to the storing formal.
BEZEL_TEST(object_string_source_retain_8307,
           as_object_alias_local_passed_to_storing_formal_spec_28_9_17_4) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); var o := x as Object; h.Put(o);"),
        kLengthTotal);
}

// The formal stored TRANSITIVELY (PutVia passes it to Put).
BEZEL_TEST(object_string_source_retain_8307,
           string_local_passed_to_transitively_storing_formal_spec_28_9) {
    expectNoUseAfterFree(storeThenRead("var x := Make.Str(i); h.PutVia(x);"),
                         kLengthTotal);
}

// An INTERFACE formal (`IEquatable<string>`, a stdlib conformance of string).
BEZEL_TEST(object_string_source_retain_8307,
           string_local_passed_to_storing_interface_formal_spec_28_9) {
    expectNoUseAfterFree(storeThenRead("var x := Make.Str(i); h.PutI(x);"),
                         kLengthTotal);
}

// A `virtual` (dispatched) storing method: no summary of an override is
// possible, so the formal escapes.
BEZEL_TEST(object_string_source_retain_8307,
           producer_passed_to_virtual_storing_formal_spec_28_9_12_1) {
    expectNoUseAfterFree(storeThenRead("h.PutV(Make.Str(i));"), kLengthTotal);
}

// The CONSTRUCTOR twin: a producer and a string local handed to an `Object`
// constructor formal the body stores.
BEZEL_TEST(object_string_source_retain_8307,
           producer_passed_to_storing_ctor_formal_spec_28_9) {
    expectNoUseAfterFree(
        program("      var h2: Holder2? := null; "
                "      do h2 := new Holder2(Make.Str(i)); end; "
                "      var junk := Make.Str(99999); "
                "      total := total + h2!.O.ToString().Length + "
                "junk.Length - junk.Length; "),
        kLengthTotal);
}

BEZEL_TEST(object_string_source_retain_8307,
           string_local_passed_to_storing_ctor_formal_spec_28_9) {
    expectNoUseAfterFree(
        program("      var h2: Holder2? := null; "
                "      do var x := Make.Str(i); h2 := new Holder2(x); end; "
                "      var junk := Make.Str(99999); "
                "      total := total + h2!.O.ToString().Length + "
                "junk.Length - junk.Length; "),
        kLengthTotal);
}

// ===========================================================================
// CONTROLS — leak-checked. Each must stay clean: the fix may not turn a
// call-bounded borrow into a leak, nor an `as` that is only READ into one.
// ===========================================================================

// A callee that only READS its `Object` formal: the #2952 summary proves it
// non-escaping, so the string local is still released at scope end.
BEZEL_TEST(object_string_source_retain_8307,
           control_string_local_to_reading_object_formal_no_leak_spec_28_3) {
    expectCleanRun(program("      do var x := Make.Str(i); "
                           "total := total + h.Peek(x); end; "),
                   kLengthTotal);
}

// The producer twin: released after the call.
BEZEL_TEST(object_string_source_retain_8307,
           control_producer_to_reading_object_formal_no_leak_spec_28_3) {
    expectCleanRun(program("      total := total + h.Peek(Make.Str(i)); "),
                   kLengthTotal);
}

// `x as Object` handed to the reading formal.
BEZEL_TEST(object_string_source_retain_8307,
           control_as_object_to_reading_object_formal_no_leak_spec_17_4) {
    expectCleanRun(program("      do var x := Make.Str(i); "
                           "total := total + h.Peek(x as Object); end; "),
                   kLengthTotal);
}

// An `as Object` alias that is only read: retained at bind and released at
// scope end (#2900's string-aliased Object local), the source released too.
BEZEL_TEST(object_string_source_retain_8307,
           control_as_object_alias_read_only_no_leak_spec_17_4_28_9) {
    expectCleanRun(program("      do var x := Make.Str(i); "
                           "var o := x as Object; "
                           "total := total + o!.ToString().Length; end; "),
                   kLengthTotal);
}

// An `as` to an interface the string conforms to, only null-tested.
BEZEL_TEST(object_string_source_retain_8307,
           control_as_interface_alias_null_test_no_leak_spec_17_4) {
    expectCleanRun(program("      do var x := Make.Str(i); "
                           "var eq := x as IEquatable<string>; "
                           "if eq <> null then total := total + 1; end; end; "),
                   std::to_string(kIterations) + "\n");
}

// A class instance through the same storing formal (the holder declared after
// the instance, so it is disposed first, §28.3): nothing about class ownership
// moves.
BEZEL_TEST(object_string_source_retain_8307,
           control_class_instance_through_storing_formal_no_leak_spec_28_3) {
    expectCleanRun(program("      var c := new Cell(); c.V := 3; "
                           "var h3 := new Holder(); h3.Put(c); "
                           "total := total + c.V; "),
                   std::to_string(3 * kIterations) + "\n");
}

// ===========================================================================
// AS-STRING ROWS — the cast back from an `Object`-typed holder
// (`<holder> as string`) reaching an `Object`-typed consumer. §17.4 (v0.357):
// "For `as`, the result is additionally a *borrow* of `x`", so it carries no
// reference of its own. Each row is a measured heap-use-after-free on the
// pre-fix build.
// ===========================================================================

// The cast handed straight to the storing `Object` formal.
BEZEL_TEST(object_string_source_retain_8307,
           as_string_of_object_local_passed_to_storing_object_formal_spec_17_4_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var o: Object := Make.Str(i); h.Put((o as string)!);"),
        kLengthTotal);
}

// The same through an `as Object` alias of a string local.
BEZEL_TEST(object_string_source_retain_8307,
           as_string_of_as_object_alias_passed_to_storing_object_formal_spec_17_4_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var x := Make.Str(i); var o := x as Object; "
                      "h.Put((o as string)!);"),
        kLengthTotal);
}

// The cast returned from an `Object`-returning method (no retain on that
// return: the declared type is not `string`).
BEZEL_TEST(object_string_source_retain_8307,
           as_string_returned_from_object_returning_method_spec_17_4_28_9) {
    expectNoUseAfterFree(
        program("      var c := Conv.AsObject(i); "
                "      var junk := Make.Str(99999); "
                "      total := total + c.ToString().Length + "
                "junk.Length - junk.Length; "),
        kLengthTotal);
}

// The cast assigned into an `Object` local that is then stored.
BEZEL_TEST(object_string_source_retain_8307,
           as_string_assigned_to_object_local_then_stored_spec_17_4_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var o: Object := Make.Str(i); var p: Object := \"\"; "
                      "p := (o as string)!; h.O := p;"),
        kLengthTotal);
}

// The cast re-cast to `Object` and stored.
BEZEL_TEST(object_string_source_retain_8307,
           as_string_recast_as_object_stored_spec_17_4_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var o: Object := Make.Str(i); "
                      "h.O := ((o as string) as Object)!;"),
        kLengthTotal);
}

// The cast BOUND to a string local (which takes its own reference at bind),
// and that local re-aliased as `Object` and stored: the bound local's own
// walk keeps its reference, so the holder may still be released.
BEZEL_TEST(object_string_source_retain_8307,
           as_string_bound_local_realiased_as_object_and_stored_spec_17_4_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var o: Object := Make.Str(i); var s := o as string; "
                      "var p := s as Object; "
                      "if p <> null then h.O := p; end;"),
        kLengthTotal);
}

// The bound string local passed on to the storing `Object` formal.
BEZEL_TEST(object_string_source_retain_8307,
           as_string_bound_local_passed_to_storing_object_formal_spec_17_4_28_9) {
    expectNoUseAfterFree(
        storeThenRead("var o: Object := Make.Str(i); var s := (o as string)!; "
                      "h.Put(s);"),
        kLengthTotal);
}

// ===========================================================================
// AS-STRING CONTROLS — leak-checked. A `string`-typed consumer takes its own
// reference, so the cast is a READ of the holder and the holder's scope-end
// release still fires. Each was clean before the fix and must stay clean.
// ===========================================================================

// The program of `codegen_string_optional_hole_producer_5290.
// cast_as_string_spec_18_5_28_9`: an interpolation hole and a `string?` bind.
BEZEL_TEST(object_string_source_retain_8307,
           control_as_string_hole_and_bind_no_leak_spec_17_4_18_5) {
    expectCleanRun(
        program("      do var o: Object := Make.Str(i); "
                "var s := $\"{(o as string)}-x\"; "
                "var b: string? := (o as string); "
                "total := total + s.Length - 2 + (b ?? \"\").Length; end; "),
        "580\n");
}

// The bind alone, annotated.
BEZEL_TEST(object_string_source_retain_8307,
           control_as_string_annotated_bind_no_leak_spec_17_4_28_9) {
    expectCleanRun(
        program("      do var o: Object := Make.Str(i); "
                "var b: string? := (o as string); "
                "total := total + (b ?? \"\").Length; end; "),
        kLengthTotal);
}

// The bind alone, inferred.
BEZEL_TEST(object_string_source_retain_8307,
           control_as_string_inferred_bind_no_leak_spec_17_4_28_9) {
    expectCleanRun(
        program("      do var o: Object := Make.Str(i); "
                "var b := o as string; "
                "total := total + (b ?? \"\").Length; end; "),
        kLengthTotal);
}

// The bound local handed to a `string?` formal the callee stores into a
// `string?` field (that store retains).
BEZEL_TEST(object_string_source_retain_8307,
           control_as_string_bind_then_string_formal_store_no_leak_spec_28_9) {
    expectCleanRun(
        program("      var k := new Sink(); "
                "      do var o: Object := Make.Str(i); "
                "var b: string? := (o as string); k.Put(b); end; "
                "      var junk := Make.Str(99999); "
                "      total := total + (k.F ?? \"\").Length + "
                "junk.Length - junk.Length; "),
        kLengthTotal);
}

// The bind of an `as Object` alias of a string local, cast back.
BEZEL_TEST(object_string_source_retain_8307,
           control_as_string_of_as_object_alias_bind_no_leak_spec_17_4_28_9) {
    expectCleanRun(
        program("      do var x := Make.Str(i); var o := x as Object; "
                "var b := o as string; "
                "total := total + (b ?? \"\").Length; end; "),
        kLengthTotal);
}

// The cast assigned to an existing string local (the assignment retains).
BEZEL_TEST(object_string_source_retain_8307,
           control_as_string_assigned_to_string_local_no_leak_spec_17_4_28_9) {
    expectCleanRun(
        program("      var b: string? := null; "
                "      do var o: Object := Make.Str(i); b := o as string; end; "
                "      var junk := Make.Str(99999); "
                "      total := total + (b ?? \"\").Length + "
                "junk.Length - junk.Length; "),
        kLengthTotal);
}

// The cast stored into a `string` field (the store retains).
BEZEL_TEST(object_string_source_retain_8307,
           control_as_string_stored_into_string_field_no_leak_spec_17_4_28_9) {
    expectCleanRun(
        program("      var k := new Sink(); "
                "      do var o: Object := Make.Str(i); "
                "k.S := (o as string)!; end; "
                "      var junk := Make.Str(99999); "
                "      total := total + k.S.Length + junk.Length - junk.Length; "),
        kLengthTotal);
}

// The cast handed to a `string` formal the callee only reads.
BEZEL_TEST(object_string_source_retain_8307,
           control_as_string_to_reading_string_formal_no_leak_spec_17_4_28_3) {
    expectCleanRun(
        program("      var k := new Sink(); "
                "      do var o: Object := Make.Str(i); "
                "total := total + k.PeekS((o as string)!); end; "),
        kLengthTotal);
}

// The cast handed to an `Object` formal the callee only reads (the #2952
// summary proves it non-escaping).
BEZEL_TEST(object_string_source_retain_8307,
           control_as_string_to_reading_object_formal_no_leak_spec_17_4_28_3) {
    expectCleanRun(
        program("      do var o: Object := Make.Str(i); "
                "total := total + h.Peek((o as string)!); end; "),
        kLengthTotal);
}

// `(x as Object)!` printed by `Console.WriteLine`, which keeps none of its
// argument (stdlib §2.1).
BEZEL_TEST(object_string_source_retain_8307,
           control_as_object_to_console_writeline_no_leak_spec_17_4_stdlib_2_1) {
    std::string expected;
    for (int i = 0; i < kIterations; ++i)
        expected += "s" + std::to_string(i) + "\n";
    expected += std::to_string(kIterations) + "\n";
    expectCleanRun(
        program("      do var x := Make.Str(i); "
                "Console.WriteLine((x as Object)!); total := total + 1; end; "),
        expected);
}
