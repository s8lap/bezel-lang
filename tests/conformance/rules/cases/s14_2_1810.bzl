//! case: s14_2_1810
//! doc: spec
//! section: 14.2
//! title: an override in a generic class inherits the constraint read under the base substitution
//! rule: inherits the overridden or implemented declaration's constraints
//! cites: 11.2
//! expect: run
//! output: 11
module Conf;

extension Object
  public method Sum(): int => 4040;
end;
class A<U>
  public virtual method S<T>(a: Array<T>, u: U): int where T: INumber<T> => a.Sum();
end A;
class D<U>: A<U>
  public override method S<T>(a: Array<T>, u: U): int => a.Sum();
end D;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D<int>();
    Console.WriteLine(d.S(a, 1));
  end Main;
end Program;
