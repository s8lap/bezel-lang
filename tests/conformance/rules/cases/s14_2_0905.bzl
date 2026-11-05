//! case: s14_2_0905
//! doc: spec
//! section: 14.2
//! title: a generic class implementing a generic interface method omitting `where` inherits its constraint
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

public interface IG<U>
  method S<T>(x: Array<T>, u: U): string where T: INumber<T>;
end IG;

public class Z<U>: IG<U>
  public constructor()
  end constructor;

  public method S<T>(x: Array<T>, u: U): string => $"{x.Sum()}";
end Z;

public class Program
  public static method Main()
    using var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    using var z := new Z<string>();
    Console.WriteLine(z.S(a, "u"));
  end Main;
end Program;
