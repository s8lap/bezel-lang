//! case: s25_2_0017
//! doc: spec
//! section: 25.2
//! title: a binary operator's interface parameter rejects an array of a non-matching element type
//! rule: "nor an overload from the candidate set above applies is a compile error at the expression"
//!   ([§25.2]). An `Array<string>` operand does not convert to `IEnumerable<int>`.
//! cites: 25.2, 5.1
//! expect: reject
//! diagnostic: is not defined for type
//! issue: 6217
module Conf;

public record V(X: int)
  public static method operator +(v: V, p: IEnumerable<int>): int => 1000;
end V;

public class Program
  public static method Main()
    var v := new V(1);
    var a: Array<string> := ["x"];
    Console.WriteLine(v + a);
  end Main;
end Program;
