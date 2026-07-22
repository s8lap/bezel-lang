// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Clean-compile ACCEPT control for issue #1847.
//
// Spec §19.3 (v0.196): the top-level bare-identifier reject added for #1847
// (checkTopLevelNamedPattern) must NOT false-reject any of the VALID top-level
// pattern forms — a genuine type test (a name that resolves to a class /
// interface / record / enum), a bare enum-constant pattern over an enum
// scrutinee (§10.6/§19.8), a named-`const` pattern (§19.3 "Constants: … a named
// `const`"), a literal, the `_` discard, and a `var` binding pattern. Every arm
// below is one of those forms; the program must compile with no diagnostics.
//
// NODIAG asserts the reject does not spuriously fire on a valid pattern.
// NODIAG-NOT: §19.3
// NODIAG-NOT: cannot be a top-level `case` pattern

module M;

enum Color
  Red, Green, Blue;
end Color;

class Animal public V: int := 0; end Animal;
class Dog: Animal end Dog;
class Cat: Animal end Cat;

class Program
  public const LIMIT: int := 42;

  // A genuine TYPE-TEST pattern (§19.3 "Type patterns without binding: … Dog").
  public static method Kind(a: Animal): string
    return (case a of
      Dog => "dog",
      Cat => "cat",
      _   => "other"
    );
  end Kind;

  // A bare ENUM-CONSTANT pattern over an enum scrutinee (§10.6 / §19.8).
  public static method Name(c: Color): string
    return (case c of
      Red   => "r",
      Green => "g",
      Blue  => "b"
    );
  end Name;

  // A named-`const` pattern, a literal pattern, a `var` binding, and `_`.
  public static method Classify(n: int): string
    return (case n of
      LIMIT      => "limit",
      0          => "zero",
      var x when x < 0 => "neg",
      _          => "pos"
    );
  end Classify;

  public static method Main()
    Console.WriteLine(Kind(new Dog()));
    Console.WriteLine(Name(Color.Green));
    Console.WriteLine(Classify(42));
  end Main;
end Program;
