// Spec-derived regression test: the fix-forward of #7003/#7004 for NESTED
// choice destructuring.
//
// Spec §19.5 (v0.340): "A typed binding pattern `var n: T` also matches only
// non-null values — the type annotation makes it a type-constrained binding,
// not an unconditional binding." §19.3 (v0.340): destructuring matching
// "proceeds like the positional form: the type test comes first (... the
// case-tag test for a choice case ...) and no slot is read on a failed type
// test"; a slot's pattern may be "a nested destructuring ... recursively".
//
// 2903bf9a made a typed class/`Object?` slot binding a runtime type test that
// reads the slot value's descriptor. A NESTED choice pattern was evaluated
// against the payload slot of an INACTIVE outer case (X's slot when the
// scrutinee is a Y), whose bytes are not a value, so the descriptor walk
// dereferenced garbage and the program crashed (SEGV) where the pre-#7003
// compiler printed the right answer. The nested slot pattern now runs only
// behind the enclosing tag match.
//
// Rows are exact-stdout. `checkedOut` rows run under `runAotChecked`;
// `plainOut` rows box an int into `Object?` (the #7005 box leak is present on
// the base compiler too) and use `runAot`.
// The probes live in /var/tmp/bezel-scratch/r34-7003ff/{p,m}/.

#include <string>

#include "support/aot_support.hpp"  // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

std::string plainOut(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

}  // namespace

BEZEL_TEST(codegen_pattern_typed_slot_nested_inactive_7003, issue_reject_repro_depth2_class_slot_inactive_outer_case) {
    BEZEL_EXPECT_EQ(
        checkedOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
choice L1 A(S: Shape?, N: int?), B(O: Object?); end L1;
choice L2 X(I: L1, K: int?), Y(I: L1); end L2;
class C
  public static method T(c: L2): string
    return (case c of
      X(A(var s: Circle, var n), var k) => "a",
      Y(B(var o)) => "d",
      Y(A(var s, var n)) => "ya",
      _ => "z");
  end T;
  public static method Main(): int
    using var c := new Circle();
    Console.WriteLine(T(L2.Y(L1.A(c, 9))));
    Console.WriteLine(T(L2.Y(L1.B(null))));
    return 0;
  end Main;
end C;)BZL"),
        std::string("ya\nd\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nested_inactive_7003, issue_reject_repro_depth3_class_slot_inactive_outer_case) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
record Pt(X: int, Y: int);

choice L1 A(S: Shape?, N: int?), B(O: Object?); end L1;
choice L2 X(I: L1, K: int?), Y(I: L1); end L2;
choice L3 P(Q: L2, S: Shape?), R; end L3;
class C
  public static method T(c: L3): string
    return (case c of
      P(X(A(var s: Circle, var n), var k), _) => "a",
      P(Y(B(var o)), _) => "d",
      P(Y(A(var s, var n)), _) => "ya",
      _ => "z");
  end T;
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method Main(): int
    using var c := new Circle();
    Console.WriteLine(T(L3.P(L2.Y(L1.B(I(9))), null)));
    Console.WriteLine(T(L3.P(L2.Y(L1.A(c, 9)), null)));
    Console.WriteLine(T(L3.P(L2.X(L1.A(c, 9), 1), null)));
    return 0;
  end Main;
end C;)BZL"),
        std::string("d\nya\na\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nested_inactive_7003, depth2_class_slot_statement_form) {
    BEZEL_EXPECT_EQ(
        checkedOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
record Pt(X: int, Y: int);
choice L1 A(V: Shape?), B(Z: int); end L1;
choice L2 X(I: L1, K: int), Y(I: L1); end L2;
choice L3 P(Q: L2), R(Q: L2); end L3;
class C
  public static method T(c: L2)
    case c of
      X(A(var v: Circle), var k) => Console.WriteLine("a" + "c"),
      Y(A(var w)) => Console.WriteLine("ya"),
      Y(B(var z)) => Console.WriteLine("yb"),
      _ => Console.WriteLine("z")
    end case;
  end T;
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method Main(): int
    using var c := new Circle();
    for var i in 0..<3 do
      T(L2.X(L1.A(c), 1));
      T(L2.Y(L1.A(c)));
      T(L2.Y(L1.B(3)));
    end;
    return 0;
  end Main;
end C;)BZL"),
        std::string("ac\nya\nyb\nac\nya\nyb\nac\nya\nyb\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nested_inactive_7003, depth3_class_slot_expression_form) {
    BEZEL_EXPECT_EQ(
        checkedOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
record Pt(X: int, Y: int);
choice L1 A(V: Shape?), B(Z: int); end L1;
choice L2 X(I: L1, K: int), Y(I: L1); end L2;
choice L3 P(Q: L2), R(Q: L2); end L3;
class C
  public static method T(c: L3): string
    return (case c of
      P(X(A(var v: Circle), var k)) => "a" + "c",
      R(Y(A(var w))) => "rya",
      R(X(B(var z), var k)) => "rxb",
      _ => "z");
  end T;
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method Main(): int
    using var c := new Circle();
    for var i in 0..<3 do
      Console.WriteLine(T(L3.P(L2.X(L1.A(c), 1))));
      Console.WriteLine(T(L3.R(L2.Y(L1.A(c)))));
      Console.WriteLine(T(L3.R(L2.X(L1.B(3), 2))));
      Console.WriteLine(T(L3.P(L2.Y(L1.A(c)))));
    end;
    return 0;
  end Main;
end C;)BZL"),
        std::string("ac\nrya\nrxb\nz\nac\nrya\nrxb\nz\nac\nrya\nrxb\nz\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nested_inactive_7003, depth3_class_slot_statement_form) {
    BEZEL_EXPECT_EQ(
        checkedOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
record Pt(X: int, Y: int);
choice L1 A(V: Shape?), B(Z: int); end L1;
choice L2 X(I: L1, K: int), Y(I: L1); end L2;
choice L3 P(Q: L2), R(Q: L2); end L3;
class C
  public static method T(c: L3)
    case c of
      P(X(A(var v: Circle), var k)) => Console.WriteLine("a" + "c"),
      R(Y(A(var w))) => Console.WriteLine("rya"),
      R(X(B(var z), var k)) => Console.WriteLine("rxb"),
      _ => Console.WriteLine("z")
    end case;
  end T;
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method Main(): int
    using var c := new Circle();
    for var i in 0..<3 do
      T(L3.P(L2.X(L1.A(c), 1)));
      T(L3.R(L2.Y(L1.A(c))));
      T(L3.R(L2.X(L1.B(3), 2)));
      T(L3.P(L2.Y(L1.A(c))));
    end;
    return 0;
  end Main;
end C;)BZL"),
        std::string("ac\nrya\nrxb\nz\nac\nrya\nrxb\nz\nac\nrya\nrxb\nz\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nested_inactive_7003, depth2_object_slot_unboxed_expression_form) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
record Pt(X: int, Y: int);
choice L1 A(V: Object?), B(Z: int); end L1;
choice L2 X(I: L1, K: int), Y(I: L1); end L2;
choice L3 P(Q: L2), R(Q: L2); end L3;
class C
  public static method T(c: L2): string
    return (case c of
      X(A(var v: int), var k) => "a" + v.ToString(),
      Y(A(var w)) => "ya",
      Y(B(var z)) => "yb",
      _ => "z");
  end T;
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method Main(): int
    using var c := new Circle();
    for var i in 0..<3 do
      Console.WriteLine(T(L2.X(L1.A(I(9)), 1)));
      Console.WriteLine(T(L2.Y(L1.A(I(9)))));
      Console.WriteLine(T(L2.Y(L1.B(3))));
    end;
    return 0;
  end Main;
end C;)BZL"),
        std::string("a9\nya\nyb\na9\nya\nyb\na9\nya\nyb\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nested_inactive_7003, depth3_object_slot_unboxed_statement_form) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
record Pt(X: int, Y: int);
choice L1 A(V: Object?), B(Z: int); end L1;
choice L2 X(I: L1, K: int), Y(I: L1); end L2;
choice L3 P(Q: L2), R(Q: L2); end L3;
class C
  public static method T(c: L3)
    case c of
      P(X(A(var v: int), var k)) => Console.WriteLine("a" + v.ToString()),
      R(Y(A(var w))) => Console.WriteLine("rya"),
      R(X(B(var z), var k)) => Console.WriteLine("rxb"),
      _ => Console.WriteLine("z")
    end case;
  end T;
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method Main(): int
    using var c := new Circle();
    for var i in 0..<3 do
      T(L3.P(L2.X(L1.A(I(9)), 1)));
      T(L3.R(L2.Y(L1.A(I(9)))));
      T(L3.R(L2.X(L1.B(3), 2)));
      T(L3.P(L2.Y(L1.A(I(9)))));
    end;
    return 0;
  end Main;
end C;)BZL"),
        std::string("a9\nrya\nrxb\nz\na9\nrya\nrxb\nz\na9\nrya\nrxb\nz\n"));
}

BEZEL_TEST(codegen_pattern_typed_slot_nested_inactive_7003, depth3_mixed_typed_slots_guards_and_object_unbox) {
    BEZEL_EXPECT_EQ(
        plainOut(
R"BZL(module M;
public class Shape end Shape;
public class Circle : Shape end Circle;
public class Sq : Shape end Sq;
record Pt(X: int, Y: int);

choice L1 A(S: Shape?, N: int?), B(O: Object?); end L1;
choice L2 X(I: L1, K: int?), Y(I: L1); end L2;
choice L3 P(Q: L2, S: Shape?), R; end L3;
class C
  public static method T(c: L3): string
    return (case c of
      P(X(A(var s: Circle, var n: int), var k: int), var t: Sq) when n > k => "a",
      P(X(A(var s: Circle, var n: int), var k: int), var t: Shape) => "b" + n.ToString() + k.ToString(),
      P(X(A(var s: Sq, var n), var k), _) => "c",
      P(Y(B(var o: int)), _) when o > 3 => "d" + o.ToString(),
      P(Y(B(var o: string)), _) => "e" + o,
      P(Y(B(var o: Pt)), var t) => "f" + o.X.ToString(),
      P(_, var t: Circle) => "g",
      _ => "z");
  end T;
  public static method PP(v: Pt): Object? var o: Object? := v; return o; end PP;
  public static method I(v: int): Object? var o: Object? := v; return o; end I;
  public static method S(v: string): Object? var o: Object? := v; return o; end S;
  public static method Main(): int
    using var c := new Circle();
    using var s := new Sq();
    Console.WriteLine(T(L3.P(L2.X(L1.A(c, 5), 3), s)));
    Console.WriteLine(T(L3.P(L2.X(L1.A(c, 2), 3), s)));
    Console.WriteLine(T(L3.P(L2.X(L1.A(c, 2), 3), c)));
    Console.WriteLine(T(L3.P(L2.X(L1.A(c, 2), null), c)));
    Console.WriteLine(T(L3.P(L2.X(L1.A(s, 2), null), c)));
    Console.WriteLine(T(L3.P(L2.Y(L1.B(I(9))), null)));
    Console.WriteLine(T(L3.P(L2.Y(L1.B(I(1))), null)));
    Console.WriteLine(T(L3.P(L2.Y(L1.B(S("h"))), null)));
    Console.WriteLine(T(L3.P(L2.Y(L1.B(PP(new Pt(8, 1)))), null)));
    Console.WriteLine(T(L3.P(L2.Y(L1.B(null)), c)));
    Console.WriteLine(T(L3.R));
    return 0;
  end Main;
end C;)BZL"),
        std::string("a\nb23\nb23\ng\nc\nd9\nz\neh\nf8\ng\nz\n"));
}
