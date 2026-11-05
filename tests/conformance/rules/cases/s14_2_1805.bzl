//! case: s14_2_1805
//! doc: spec
//! section: 14.2
//! title: a method overriding a virtual and implementing an interface with equal sets inherits them
//! rule: inherits the overridden or implemented declaration's constraints
//! cites: 11.2
//! expect: run
//! output: 11
module Conf;

extension Object
  public method Sum(): int => 4040;
end;
interface IA
  method S<T>(a: Array<T>): int where T: INumber<T>;
end IA;
class B
  public virtual method S<T>(a: Array<T>): int where T: INumber<T> => a.Sum();
end B;
class D: B, IA
  public override method S<T>(a: Array<T>): int => a.Sum();
end D;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D();
    Console.WriteLine(d.S(a));
  end Main;
end Program;
