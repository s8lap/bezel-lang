//! case: s8_5_2305
//! doc: spec
//! section: 8.5
//! title: an inline array of a generic class instance recurses into the
//!   type arguments
//! rule: the element recursing in that form, as the class bullet's type
//!   arguments do): `"[2]M.Rec"` for an element type `Rec` declared in
//!   module `M`
//! cites: 2.13
//! expect: run
//! output: [2]Conf.Box<Conf.Rec>
//! issue: 7794
//! notes: Red on base: the pre-fix compiler printed the element unqualified
//!   (`[2]Box`), the staging tip prints the FullName rendering. (the
//!   pre-fix compiler also dropped the type arguments).
module Conf;

public record Rec(X: int);

public class Box<T>
  public constructor() end constructor;
end Box;

public class Program
  public static method Main()
    using var b1 := new Box<Rec>();
    using var b2 := new Box<Rec>();
    var a: [2]Box<Rec> := [b1, b2];
    Console.WriteLine(a.ToString());
  end Main;
end Program;
