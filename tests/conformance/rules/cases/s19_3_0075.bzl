//! case: s19_3_0075
//! doc: spec
//! section: 19.3
//! title: nested choice patterns select the first arm whose every case tag and typed slot
//!   matches
//! rule: "the **type test comes first** (... the case-tag test for a choice case; a named
//!   pattern therefore never matches `null`, and no slot is read on a failed type test)"
//!   (§19.3, v0.340); arms are tried in order and a failed tag or typed-slot test at any
//!   depth moves on to the next arm. `Y(L1.A(c, 9))` fails the outer `X` tag, then the inner
//!   `B` tag, and matches the third arm; `Y(L1.B(null))` matches the second.
//! cites: 35.5, 19.5
//! expect: run
//! output: ya
//!   d
module Conf;

public class Shape end Shape;
public class Circle : Shape end Circle;

public choice L1
  A(S: Shape?, N: int?),
  B(O: Object?);
end L1;

public choice L2
  X(I: L1, K: int?),
  Y(I: L1);
end L2;

public class Program
  public static method T(c: L2): string
    return (case c of
      X(A(var s: Circle, var n), var k) => "a",
      Y(B(var o)) => "d",
      Y(A(var s, var n)) => "ya",
      _ => "z");
  end T;

  public static method Main()
    using var c := new Circle();
    Console.WriteLine(T(L2.Y(L1.A(c, 9))));
    Console.WriteLine(T(L2.Y(L1.B(null))));
  end Main;
end Program;
