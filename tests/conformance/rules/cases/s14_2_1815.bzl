//! case: s14_2_1815
//! doc: spec
//! section: 14.2
//! title: method type parameters correspond by position and only the constrained position inherits
//! rule: The two declarations' method-level type parameters, equal in number,
//!   correspond by position
//! cites: 11.2
//! expect: run
//! output: 404000011
module Conf;

extension Object
  public method Sum(): int => 4040;
end;
interface IA
  method S<A, Z>(a: Array<A>, b: Array<Z>): int where Z: INumber<Z>;
end IA;
class D: IA
  public method S<X, Y>(a: Array<X>, b: Array<Y>): int => a.Sum() * 100000 + b.Sum();
end D;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D();
    Console.WriteLine(d.S(a, a));
  end Main;
end Program;
