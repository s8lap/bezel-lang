//! case: s12_2_1801
//! doc: spec
//! section: 12.2
//! title: a private base method of the same name does not stop a method inheriting a constraint
//! rule: a `private` base member never blocks
//! cites: 14.2
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
  private method S<T>(a: Array<T>): int where T: INumber<T> => 0;
  public method K(): int => 0;
end B;
class D: B, IA
  public method S<T>(a: Array<T>): int => a.Sum();
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
