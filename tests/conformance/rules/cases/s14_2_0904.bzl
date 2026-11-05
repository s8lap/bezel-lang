//! case: s14_2_0904
//! doc: spec
//! section: 14.2
//! title: a static generic interface method's implementation omitting `where` inherits its constraint
//! rule: inherits the overridden or implemented declaration's constraints. Omitting the `where`
//!   clauses is the recommended spelling and means exactly the inherited set
//! cites: 26.5, 11.2
//! expect: run
//! output: 11
//! issue: 8180
module Conf;

public extension Object
  public method Sum(): string => "ext";
end extension;

public interface IA
  static method S<T>(x: Array<T>): string where T: INumber<T>;
end IA;

public class D: IA
  public static method S<T>(x: Array<T>): string => $"{x.Sum()}";
end D;

public class Program
  public static method Main()
    using var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    Console.WriteLine(D.S(a));
  end Main;
end Program;
