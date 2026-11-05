//! case: s14_2_1811
//! doc: spec
//! section: 14.2
//! title: a constrained interface method's implementation inherits, a sibling's unconstrained one does not
//! rule: inherits the overridden or implemented declaration's constraints
//! cites: 11.2
//! expect: run
//! output: 11
//!   4040
module Conf;

extension Object
  public method Sum(): int => 4040;
end;
interface IA
  method S<T>(a: Array<T>): int where T: INumber<T>;
end IA;
interface IB
  method S<T>(s: string, a: Array<T>): int;
end IB;
class D: IB, IA
  public method S<T>(s: string, a: Array<T>): int => a.Sum();
  public method S<T>(a: Array<T>): int => a.Sum();
end D;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D();
    Console.WriteLine(d.S(a));
    Console.WriteLine(d.S("k", a));
  end Main;
end Program;
