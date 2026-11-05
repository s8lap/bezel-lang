//! case: s14_2_1814
//! doc: spec
//! section: 14.2
//! title: an override chain from an interface-implementing virtual inherits throughout
//! rule: In every comparison this paragraph makes, a method's **constraint set** is
//!   its stated clauses or, where it omits them, the set it inherits here.
//! cites: 11.2
//! expect: run
//! output: 11
//!   11
module Conf;

extension Object
  public method Sum(): int => 4040;
end;
interface IA
  method S<T>(a: Array<T>): int where T: INumber<T>;
end IA;
class B: IA
  public virtual method S<T>(a: Array<T>): int => a.Sum();
end B;
class M: B
  public override method S<T>(a: Array<T>): int => a.Sum();
end M;
class D: M, IA
  public override method S<T>(a: Array<T>): int => a.Sum();
end D;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D();
    Console.WriteLine(d.S(a));
    var m := new M();
    Console.WriteLine(m.S(a));
  end Main;
end Program;
