//! case: s14_2_0903
//! doc: spec
//! section: 14.2
//! title: a record's implementation of a generic interface method omitting `where` inherits its constraint
//! rule: inherits the overridden or implemented declaration's constraints. Omitting the `where`
//!   clauses is the recommended spelling and means exactly the inherited set
//! cites: 26.5, 11.2
//! expect: run
//! output: 11
//!   11
//! issue: 8180
module Conf;

public extension Object
  public method Sum(): string => "ext";
end extension;

public interface IA
  method S<T>(x: Array<T>): string where T: INumber<T>;
end IA;

public record R(V: int): IA
  public method S<T>(x: Array<T>): string => $"{x.Sum()}";
end R;

public class Program
  public static method Main()
    using var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var r := new R(1);
    Console.WriteLine(r.S(a));
    var ri: IA := r;
    Console.WriteLine(ri.S(a));
  end Main;
end Program;
