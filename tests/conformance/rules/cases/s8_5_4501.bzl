//! case: s8_5_4501
//! doc: spec
//! section: 8.5
//! title: ToString on a bare type parameter at int renders the concrete text in every
//!   expression position
//! rule: a `T`-typed operand — a receiver or interpolation hole whose static type is
//!   a type parameter
//! cites: 14.2
//! expect: run
//! output: 42
//!   2
//!   rec 2 42
//!   <42>
//!   42
//!   42
//! issue: 6933
module Conf;

public class Prog
  static method Show<T>(x: T): string => x.ToString();
  static method Len<T>(x: T): int => x.ToString().Length;
  static method Rec<T>(x: T)
    Console.WriteLine($"rec {x.ToString().Length} {x.ToString()}");
  end Rec;
  static method Interp<T>(x: T): string => $"<{x}>";
  static method Lam<T>(x: T): string
    var f := lambda() => x.ToString();
    return f();
  end Lam;
  public static method Main(): int
    var v: int := 42;
    Console.WriteLine(Show(v));
    Console.WriteLine(Len(v));
    Rec(v);
    Console.WriteLine(Interp(v));
    Console.WriteLine(Lam(v));
    Console.WriteLine(v.ToString());
    return 0;
  end Main;
end Prog;
