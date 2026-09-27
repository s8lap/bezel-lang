// fix #5068 (§9.1 / §9.2 / §8.3, stdlib §2.9, v0.307): a record that LISTS
// `IEquatable<Self>` must answer `Equals(T)` with the synthesized field-by-
// field equality — the same answer as the unlisted record and as `=`.
//
// §9.1 (v0.307): the compiler generates "`Equals(Object?)` and `Equals(T)`
// over the same participating positional fields, agreeing with the generated
// `=` (§8.3); the synthesized `Equals(T)` is what satisfies a declared
// `IEquatable<T>` conformance (§14.2)". stdlib §2.9: `IEquatable<T>` declares
// `method Equals(other: T): bool`; "Records implement this implicitly".
//
// ROOT CAUSE. The synthesized `Equals(T)` is minted in codegen and is never a
// `TypeSymbol::members` entry, so once a record lists `IEquatable<Self>` (made
// compilable by #5048) sema resolves `p.Equals(q)` to the ABSTRACT interface
// member. Three consumers then asked "which member implements this?" and found
// nothing:
//   (1) tryLowerValueSemanticsCall bailed on ANY resolved MethodSymbol, so a
//       record-VALUE receiver went down the interface-dispatch path, which
//       cannot take one and degraded to a constant `false` (measured on
//       28bd7e5f: the issue's repro exits 1; the unlisted record exits 0);
//   (2) the boxed record's `IEquatable` itable slot stayed NULL, so a call
//       through an `IEquatable<Point>` reference called address 0 (SIGSEGV,
//       rc=139 measured);
//   (3) the constrained-generic spelling `Gen<Point>(a, b)` with
//       `where T: IEquatable<T>` hit (1) inside the monomorphized body.
// Plus the generic-record twin of (2): the shared itmap thunk is compiled
// against the template layout, so a `Pair<string>` needs a per-instantiation
// slot (the same swap #2176 does for the vtable Equals slot).
//
// All rows run on runAot (the strict AOT oracle). Each row's expected value is
// a bitmask of distinct observations so a partial regression names itself.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    JitResult r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

const std::string kDecls =
    "module M; "
    "record Point(X: int, Y: int): IEquatable<Point>; "
    "record Named(Name: string, N: int): IEquatable<Named>; "
    "record Pair<T>(A: T, B: T): IEquatable<Pair<T>>; "
    // An auto-hashable BODY record listing IEquatable<Self> (§9.2).
    "record Label: IEquatable<Label> "
    "  public property Name: string; "
    "  public constructor(n: string) Name := n; end constructor; "
    "end Label; "
    // Control: a record whose OWN Equals(T) is the witness. It compares Name
    // only, so it disagrees with field-by-field equality on purpose.
    "record Person: IEquatable<Person> "
    "  public property Name: string; "
    "  public property Age: int; "
    "  public constructor(n: string, a: int) Name := n; Age := a; "
    "  end constructor; "
    "  public method Equals(other: Person): bool => Name = other.Name; "
    "  public method GetHashCode(): int => Name.GetHashCode(); "
    "end Person; ";

std::string prog(const std::string& helpers, const std::string& body) {
    return kDecls + "class App " + helpers +
           "  public static method Main(): int "
           "    var r := 0; " + body + " return r; "
           "  end Main; "
           "end App;";
}

}  // namespace

// The issue's own repro (its `let` locals spelled `var`, and a `module`
// header added, so it compiles): equal values answer true.
BEZEL_TEST(record_iequatable_self_equals_5068,
           issue_repro_equal_values_exit_0_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Point(X: int, Y: int): IEquatable<Point>; "
        "class App "
        "  public static method Main(): int "
        "    var p1 := new Point(1, 2); "
        "    var p2 := new Point(1, 2); "
        "    if p1.Equals(p2) then return 0; else return 1; end if; "
        "  end Main; "
        "end App;"), 0);
}

// Direct call on a positional record: equal → true, unequal → false, and the
// answer agrees with `=` (§9.1 "agreeing with the generated `=`").
BEZEL_TEST(record_iequatable_self_equals_5068,
           direct_positional_equal_true_unequal_false_agrees_with_eq_spec_9_1) {
    BEZEL_EXPECT_EQ(run(prog("",
        "var p1 := new Point(1, 2); var p2 := new Point(1, 2); "
        "var p3 := new Point(1, 3); "
        "if p1.Equals(p2) then r := r + 1; end if; "
        "if not p1.Equals(p3) then r := r + 2; end if; "
        "if p1.Equals(p2) = (p1 = p2) then r := r + 4; end if; "
        "if p1.Equals(p3) = (p1 = p3) then r := r + 8; end if;")), 15);
}

// A string field compares by CONTENT: the equal operand's string is built at
// run time, so a pointer comparison would answer false.
BEZEL_TEST(record_iequatable_self_equals_5068,
           direct_string_field_compares_by_content_spec_9_1_8_3) {
    BEZEL_EXPECT_EQ(run(prog("",
        "var s := \"a\"; var t := s + \"b\"; "
        "var n1 := new Named(\"ab\", 1); var n2 := new Named(t, 1); "
        "var n3 := new Named(\"ac\", 1); var n4 := new Named(\"ab\", 2); "
        "if n1.Equals(n2) then r := r + 1; end if; "
        "if not n1.Equals(n3) then r := r + 2; end if; "
        "if not n1.Equals(n4) then r := r + 4; end if;")), 7);
}

// A generic record listing IEquatable<Pair<T>>, at an int and a string
// instantiation.
BEZEL_TEST(record_iequatable_self_equals_5068,
           direct_generic_record_int_and_string_spec_9_1_14_1) {
    BEZEL_EXPECT_EQ(run(prog("",
        "var q1 := new Pair<int>(1, 2); var q2 := new Pair<int>(1, 2); "
        "var q3 := new Pair<int>(2, 2); "
        "if q1.Equals(q2) then r := r + 1; end if; "
        "if not q1.Equals(q3) then r := r + 2; end if; "
        "var s := \"x\"; var t := s + \"y\"; "
        "var w1 := new Pair<string>(\"x\", t); "
        "var w2 := new Pair<string>(\"x\", \"xy\"); "
        "var w3 := new Pair<string>(\"x\", \"z\"); "
        "if w1.Equals(w2) then r := r + 4; end if; "
        "if not w1.Equals(w3) then r := r + 8; end if;")), 15);
}

// An auto-hashable BODY record (§9.2) listing IEquatable<Self>.
BEZEL_TEST(record_iequatable_self_equals_5068,
           direct_body_record_auto_hashable_spec_9_2) {
    BEZEL_EXPECT_EQ(run(prog("",
        "var l1 := new Label(\"k\"); var l2 := new Label(\"k\"); "
        "var l3 := new Label(\"j\"); "
        "if l1.Equals(l2) then r := r + 1; end if; "
        "if not l1.Equals(l3) then r := r + 2; end if;")), 3);
}

// THE ITABLE PATH: dispatch through a borrowed `IEquatable<Point>` parameter.
// Pre-fix the boxed record's IEquatable slot was null → SIGSEGV.
BEZEL_TEST(record_iequatable_self_equals_5068,
           itable_positional_record_through_iequatable_param_spec_9_1_stdlib_2_9) {
    BEZEL_EXPECT_EQ(run(prog(
        "  public static method Check(a: IEquatable<Point>, b: Point): bool "
        "    => a.Equals(b); ",
        "if Check(new Point(1, 2), new Point(1, 2)) then r := r + 1; end if; "
        "if not Check(new Point(1, 2), new Point(1, 3)) then r := r + 2; end if;")),
        3);
}

// The itable path with a reference-typed (string) field and with an
// auto-hashable body record.
BEZEL_TEST(record_iequatable_self_equals_5068,
           itable_string_field_and_body_record_spec_9_1_9_2_stdlib_2_9) {
    BEZEL_EXPECT_EQ(run(prog(
        "  public static method CheckN(a: IEquatable<Named>, b: Named): bool "
        "    => a.Equals(b); "
        "  public static method CheckL(a: IEquatable<Label>, b: Label): bool "
        "    => a.Equals(b); ",
        "var s := \"a\"; var t := s + \"b\"; "
        "var n1 := new Named(\"ab\", 1); var n2 := new Named(t, 1); "
        "var n3 := new Named(\"ac\", 1); "
        "if CheckN(n1, n2) then r := r + 1; end if; "
        "if not CheckN(n1, n3) then r := r + 2; end if; "
        "var l1 := new Label(\"k\"); var l2 := new Label(\"k\"); "
        "var l3 := new Label(\"j\"); "
        "if CheckL(l1, l2) then r := r + 4; end if; "
        "if not CheckL(l1, l3) then r := r + 8; end if;")), 15);
}

// The itable path on a generic record at a STRING instantiation: the slot
// must be compiled against Pair<string>'s layout, not the erased template.
BEZEL_TEST(record_iequatable_self_equals_5068,
           itable_generic_record_string_instantiation_spec_14_1_stdlib_2_9) {
    BEZEL_EXPECT_EQ(run(prog(
        "  public static method CheckP(a: IEquatable<Pair<string>>, "
        "                              b: Pair<string>): bool => a.Equals(b); ",
        "var s := \"x\"; var t := s + \"y\"; "
        "var w1 := new Pair<string>(\"x\", t); "
        "var w2 := new Pair<string>(\"x\", \"xy\"); "
        "var w3 := new Pair<string>(\"x\", \"z\"); "
        "if CheckP(w1, w2) then r := r + 1; end if; "
        "if not CheckP(w1, w3) then r := r + 2; end if;")), 3);
}

// The constrained-generic spelling: `where T: IEquatable<T>` bound to a record
// whose witness is synthesized (stdlib §9.1 note: "when an
// `IEquatable<int>`-constrained generic uses `Equals` through the constraint").
BEZEL_TEST(record_iequatable_self_equals_5068,
           constrained_generic_bound_to_listing_record_spec_14_2_stdlib_2_9) {
    BEZEL_EXPECT_EQ(run(prog(
        "  public static method Gen<T>(a: T, b: T): bool "
        "    where T: IEquatable<T> => a.Equals(b); ",
        "if Gen<Point>(new Point(1, 2), new Point(1, 2)) then r := r + 1; end if; "
        "if not Gen<Point>(new Point(1, 2), new Point(3, 3)) then r := r + 2; end if; "
        "var s := \"a\"; var t := s + \"b\"; "
        "var n1 := new Named(\"ab\", 1); var n2 := new Named(t, 1); "
        "var n3 := new Named(\"ac\", 1); "
        "if Gen<Named>(n1, n2) then r := r + 4; end if; "
        "if not Gen<Named>(n1, n3) then r := r + 8; end if;")), 15);
}

// CONTROL: a record whose own `Equals(T)` is the IEquatable witness keeps its
// user semantics on all three spellings (direct, itable, constrained generic).
// Person compares Name only, so Person("p", 1) equals Person("p", 2).
BEZEL_TEST(record_iequatable_self_equals_5068,
           control_user_equals_witness_wins_on_every_path_spec_9_2_14_2) {
    BEZEL_EXPECT_EQ(run(prog(
        "  public static method CheckPe(a: IEquatable<Person>, b: Person): bool "
        "    => a.Equals(b); "
        "  public static method Gen<T>(a: T, b: T): bool "
        "    where T: IEquatable<T> => a.Equals(b); ",
        "var e1 := new Person(\"p\", 1); var e2 := new Person(\"p\", 2); "
        "var e3 := new Person(\"q\", 1); "
        "if e1.Equals(e2) then r := r + 1; end if; "
        "if not e1.Equals(e3) then r := r + 2; end if; "
        "if CheckPe(e1, e2) then r := r + 4; end if; "
        "if not CheckPe(e1, e3) then r := r + 8; end if; "
        "if Gen<Person>(e1, e2) then r := r + 16; end if; "
        "if not Gen<Person>(e1, e3) then r := r + 32; end if;")), 63);
}

// `Equals` with an `Object` argument holding a box of an equal record (§8.1 /
// §8.6): the listed record answers exactly as the unlisted one, and the
// argument's box is still released at scope end (loop-amplified so a leaked
// box per iteration is visible to the ASan leg).
BEZEL_TEST(record_iequatable_self_equals_5068,
           object_argument_boxed_equal_record_spec_8_1_8_6) {
    BEZEL_EXPECT_EQ(run(prog("",
        "var hits := 0; var i := 0; "
        "while i < 200 do "
        "  var p := new Point(1, 2); "
        "  var o: Object := new Point(1, 2); "
        "  var o2: Object := new Point(1, 3); "
        "  if p.Equals(o) and not p.Equals(o2) then hits := hits + 1; end if; "
        "  i := i + 1; "
        "end while; "
        "if hits = 200 then r := 1; end if;")), 1);
}
