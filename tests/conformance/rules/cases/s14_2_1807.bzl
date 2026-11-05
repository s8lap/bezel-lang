//! case: s14_2_1807
//! doc: spec
//! section: 14.2
//! title: a static method implementing an interface static method inherits its constraints
//! rule: inherits the overridden or implemented declaration's constraints
//! cites: 11.2
//! expect: run
//! output: 11
module Conf;

extension Object
  public method Sum(): int => 4040;
end;
interface IA
  static method S<T>(a: Array<T>): int where T: INumber<T>;
end IA;
class D: IA
  public static method S<T>(a: Array<T>): int => a.Sum();
end D;
class Program
  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    Console.WriteLine(D.S(a));
  end Main;
end Program;
