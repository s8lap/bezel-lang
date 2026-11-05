//! case: s14_2_0902
//! doc: spec
//! section: 14.2
//! title: an override with renamed type parameters inherits the constraint by position, not by name
//! rule: inherits the overridden or implemented declaration's constraints. Omitting the `where`
//!   clauses is the recommended spelling and means exactly the inherited set ... The two
//!   declarations' method-level type parameters, equal in number, correspond by position
//! cites: 26.5
//! expect: run
//! output: 11|ext
//! issue: 8180
module Conf;

public extension Object
  public method Sum(): string => "ext";
end extension;

public class B
  public virtual method S<A, Z>(x: Array<A>, y: Array<Z>): string where A: INumber<A> => $"{x.Sum()}";
end B;

public class D: B
  public override method S<Z, A>(x: Array<Z>, y: Array<A>): string => $"{x.Sum()}|{y.Sum()}";
end D;

public class Program
  public static method Main()
    using var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    using var d := new D();
    Console.WriteLine(d.S(a, a));
  end Main;
end Program;
