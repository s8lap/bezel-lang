//! case: s8_5_4504
//! doc: spec
//! section: 8.5
//! title: ToString on a bare type parameter at a class with an override dispatches to
//!   the override
//! rule: `ToString` is invoked implicitly inside string interpolation
//!   ([§18.5](#sec-interpolated-strings)) via virtual dispatch
//! cites: 14.2, 8.1
//! expect: run
//! output: widget
//!   6
//!   rec 6 widget
//!   <widget>
//!   widget
//!   widget
//! issue: 6933
module Conf;

public class W
  public override method ToString(): string => "widget";
  destructor
  end destructor;
end W;

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
    using var v: W := new W();
    Console.WriteLine(Show(v));
    Console.WriteLine(Len(v));
    Rec(v);
    Console.WriteLine(Interp(v));
    Console.WriteLine(Lam(v));
    Console.WriteLine(v.ToString());
    return 0;
  end Main;
end Prog;
