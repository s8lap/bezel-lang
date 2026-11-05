//! case: s14_2_0906
//! doc: spec
//! section: 14.2
//! title: an override two abstract levels up omitting `where` inherits the abstract method's constraint
//! rule: inherits the overridden or implemented declaration's constraints. Omitting the `where`
//!   clauses is the recommended spelling and means exactly the inherited set
//! cites: 26.5, 12.1
//! expect: run
//! output: 11
//! issue: 8180
module Conf;

public extension Object
  public method Sum(): string => "ext";
end extension;

public abstract class A
  public abstract method S<T>(x: Array<T>): string where T: INumber<T>;
end A;

public abstract class M2: A
  public method K(): int => 0;
end M2;

public class D: M2
  public constructor()
  end constructor;

  public override method S<T>(x: Array<T>): string => $"{x.Sum()}";
end D;

public class Program
  public static method Main()
    using var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    using var d := new D();
    Console.WriteLine(d.S(a));
  end Main;
end Program;
