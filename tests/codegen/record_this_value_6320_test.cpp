// Regression tests for #6320 (§9.4 records are value types / §35.1 choices
// are value types, spec v0.338): the residual sinks of #6317. In a record or
// choice instance method `this` lowered to the receiver's ADDRESS (CG29), and
// every value position that did not load it by hand mis-copied the pointer:
// `a.Insert(0, this)` and `a[0] := this` stored garbage, a Dictionary Add or
// indexer set of `this` crashed, an `Object` box of `this` boxed the pointer,
// and a lambda returning `this`, or `this` reaching a `Tag?` parameter or
// return, failed IR verification. The fix lowers `this` as the loaded VALUE in
// every expression position, keeping the address only where a member access,
// a method call on `this` or a `this with` needs it.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

const char* kTag =
    "module M; "
    "public record Tag "
    "  public readonly property N: int; "
    "  public constructor(n: int) N := n; end constructor; ";

const char* kChoice =
    "module M; "
    "public choice C "
    "  A(x: int), "
    "  B(y: int); ";
}  // namespace

// `a.Insert(0, this)` (issue p08): the inserted element read back garbage.
BEZEL_TEST(record_this_value_6320, record_insert_this_into_array) {
    BEZEL_EXPECT_EQ(runFull(std::string(kTag) +
        "  public method S(a: Array<Tag>) a.Insert(0, this); end S; "
        "end Tag; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new Array<Tag>(); a.Add(new Tag(1)); "
        "    var t := new Tag(7); t.S(a); "
        "    return a[0].N * 10 + a[1].N; "
        "  end Main; "
        "end Program;"), 71);
}

// `a[0] := this` (issue p09).
BEZEL_TEST(record_this_value_6320, record_indexer_set_this_into_array) {
    BEZEL_EXPECT_EQ(runFull(std::string(kTag) +
        "  public method S(a: Array<Tag>) a[0] := this; end S; "
        "end Tag; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new Array<Tag>(); a.Add(new Tag(1)); "
        "    var t := new Tag(7); t.S(a); "
        "    return a[0].N; "
        "  end Main; "
        "end Program;"), 7);
}

// `d.Add(k, this)` (issue p06): the boxed dictionary slot held the receiver's
// stack address, freed at dictionary disposal (bad-free / SIGABRT).
BEZEL_TEST(record_this_value_6320, record_dictionary_add_this) {
    BEZEL_EXPECT_EQ(runFull(std::string(kTag) +
        "  public method S(d: Dictionary<int, Tag>, k: int) d.Add(k, this); end S; "
        "end Tag; "
        "public class Program "
        "  public static method Main(): int "
        "    var d := new Dictionary<int, Tag>(); var t := new Tag(7); "
        "    for var i in 0..<20 do t.S(d, i); end; "
        "    return d[13].N + d.Count; "
        "  end Main; "
        "end Program;"), 27);
}

// `d[k] := this` (issue p07).
BEZEL_TEST(record_this_value_6320, record_dictionary_indexer_set_this) {
    BEZEL_EXPECT_EQ(runFull(std::string(kTag) +
        "  public method S(d: Dictionary<int, Tag>, k: int) d[k] := this; end S; "
        "end Tag; "
        "public class Program "
        "  public static method Main(): int "
        "    var d := new Dictionary<int, Tag>(); var t := new Tag(7); "
        "    for var i in 0..<20 do t.S(d, i mod 4); end; "
        "    return d[3].N + d.Count; "
        "  end Main; "
        "end Program;"), 11);
}

// `var o: Object := this` (issue p36): the box was minted from the pointer.
BEZEL_TEST(record_this_value_6320, record_object_box_of_this) {
    BEZEL_EXPECT_EQ(runFull(std::string(kTag) +
        "  public method S(a: Array<Object>) var o: Object := this; a.Add(o); end S; "
        "end Tag; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new Array<Object>(); var t := new Tag(7); t.S(a); "
        "    var o := a[0]; "
        "    if o is Tag then return (o as Tag)!.N; end if; "
        "    return 1; "
        "  end Main; "
        "end Program;"), 7);
}

// `this` returned where `Object` is declared.
BEZEL_TEST(record_this_value_6320, record_return_this_as_object) {
    BEZEL_EXPECT_EQ(runFull(std::string(kTag) +
        "  public method Box(): Object => this; "
        "end Tag; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := new Tag(7); var o := t.Box(); "
        "    if o is Tag then return (o as Tag)!.N; end if; "
        "    return 1; "
        "  end Main; "
        "end Program;"), 7);
}

// A lambda returning `this` (IR verification failure: `ret ptr`).
BEZEL_TEST(record_this_value_6320, record_lambda_returns_this) {
    BEZEL_EXPECT_EQ(runFull(std::string(kTag) +
        "  public method S(a: Array<Tag>) var f := lambda(): Tag => this; a.Add(f()); end S; "
        "end Tag; "
        "public class Program "
        "  public static method Main(): int "
        "    var a := new Array<Tag>(); var t := new Tag(7); t.S(a); "
        "    return a[0].N; "
        "  end Main; "
        "end Program;"), 7);
}

// `this` passed to a `Tag?` parameter (issue p39).
BEZEL_TEST(record_this_value_6320, record_this_to_nullable_parameter) {
    BEZEL_EXPECT_EQ(runFull(std::string(kTag) +
        "  public method S(): int => Q(this); "
        "  private static method Q(x: Tag?): int => (if x = null then 1 else x!.N); "
        "end Tag; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := new Tag(7); return t.S(); "
        "  end Main; "
        "end Program;"), 7);
}

// `this` returned as `Tag?` (issue p40).
BEZEL_TEST(record_this_value_6320, record_this_returned_as_nullable) {
    BEZEL_EXPECT_EQ(runFull(std::string(kTag) +
        "  public method Me(): Tag? => this; "
        "end Tag; "
        "public class Program "
        "  public static method Main(): int "
        "    var t := new Tag(7); return t.Me()!.N; "
        "  end Main; "
        "end Program;"), 7);
}

// Choice: `d[k] := this` (issue p25, SIGSEGV).
BEZEL_TEST(record_this_value_6320, choice_dictionary_indexer_set_this) {
    BEZEL_EXPECT_EQ(runFull(std::string(kChoice) +
        "  public method S(d: Dictionary<int, C>) d[1] := this; end S; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var d := new Dictionary<int, C>(); var t: C := C.A(7); t.S(d); "
        "    var r := 0; "
        "    case d[1] of A(x) => r := x, B(y) => r := y + 100; end case; "
        "    return r; "
        "  end Main; "
        "end Program;"), 7);
}

// Choice: `d.Add(k, this)` and `a.Insert(0, this)`.
BEZEL_TEST(record_this_value_6320, choice_dictionary_add_and_insert_this) {
    BEZEL_EXPECT_EQ(runFull(std::string(kChoice) +
        "  public method S(d: Dictionary<int, C>, a: Array<C>) "
        "    d.Add(1, this); a.Insert(0, this); "
        "  end S; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var d := new Dictionary<int, C>(); var a := new Array<C>(); "
        "    a.Add(C.B(1)); var t: C := C.A(7); t.S(d, a); "
        "    var r := 0; "
        "    case d[1] of A(x) => r := x, B(y) => r := y + 100; end case; "
        "    case a[0] of A(x) => r := r + x * 10, B(y) => r := r + 200; end case; "
        "    return r; "
        "  end Main; "
        "end Program;"), 77);
}

// Choice: `this` boxed as `Object`, returned through a lambda, and returned as
// `C?`.
BEZEL_TEST(record_this_value_6320, choice_this_object_lambda_and_nullable) {
    BEZEL_EXPECT_EQ(runFull(std::string(kChoice) +
        "  public method Box(): Object => this; "
        "  public method L(): C "
        "    var f := lambda(): C => this; "
        "    return f(); "
        "  end L; "
        "  public method MeN(): C? => this; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    var t: C := C.A(7); var r := 0; "
        "    if t.Box() is C then r := 1; end if; "
        "    case t.L() of A(x) => r := r + (if x = 7 then 2 else 0), B(y) => r := r + 0; end case; "
        "    case t.MeN()! of A(x) => r := r + (if x = 7 then 4 else 0), B(y) => r := r + 0; end case; "
        "    return r; "
        "  end Main; "
        "end Program;"), 7);
}
