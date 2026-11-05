//! case: s14_2_1808
//! doc: spec
//! section: 14.2
//! title: a record method implementing a generic interface method inherits its constraints
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
record R(x: int): IA
  public method S<T>(a: Array<T>): int => a.Sum();
end R;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var r := new R(1);
    Console.WriteLine(r.S(a));
  end Main;
end Program;
