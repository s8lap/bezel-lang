//! case: s8_4_0006
//! doc: spec
//! section: 8.4
//! title: a present record or class compared with null is unequal on either side
//! rule: "`a = b` invokes `a.Equals(b)`, with a null-safe wrapper: `null = null` is `true`;
//!   `null = nonNull` and `nonNull = null` are `false`. `<>` is the negation." (§8.4,
//!   v0.340) A present record value and a present class instance are both `nonNull`, so `=
//!   null` and `null =` are `false` and `<>` is `true`.
//! cites: 7.1
//! expect: run
//! output: False
//!   True
//!   False
//!   True
//!   False
//!   True
//!   False
//!   True
module Conf;

public record Pt(X: int, Y: int);
public class C end C;

public class Program
  public static method Main()
    var p := new Pt(1, 2);
    Console.WriteLine(p = null);
    Console.WriteLine(p <> null);
    Console.WriteLine(null = p);
    Console.WriteLine(null <> p);
    var c := new C();
    Console.WriteLine(c = null);
    Console.WriteLine(c <> null);
    Console.WriteLine(null = c);
    Console.WriteLine(null <> c);
  end Main;
end Program;
