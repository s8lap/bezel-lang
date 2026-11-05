//! case: s14_2_1809
//! doc: spec
//! section: 14.2
//! title: an override of an abstract generic method inherits its constraints
//! rule: inherits the overridden or implemented declaration's constraints
//! cites: 11.2
//! expect: run
//! output: 11
module Conf;

extension Object
  public method Sum(): int => 4040;
end;
abstract class A
  public abstract method S<T>(a: Array<T>): int where T: INumber<T>;
end A;
class D: A
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
