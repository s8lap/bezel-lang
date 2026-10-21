//! case: s25_2_0023
//! doc: spec
//! section: 25.2
//! title: a relational operator rejects an operand that matches none of its overloads
//! rule: "nor an overload from the candidate set above applies is a compile error at the expression"
//!   ([§25.2]). A `StringBuilder` operand converts to no relational overload parameter.
//! cites: 25.2, 5.1
//! expect: reject
//! diagnostic: is not defined for type
//! issue: 6217
module Conf;

public record V(X: int)
  public static method operator <(v: V, p: IEnumerable<int>): bool => true;
  public static method operator >(v: V, p: IEnumerable<int>): bool => false;
  public static method operator <=(v: V, p: IEnumerable<int>): bool => true;
  public static method operator >=(v: V, p: IEnumerable<int>): bool => false;
end V;

public class Program
  public static method Main()
    var v := new V(1);
    using var s := new StringBuilder();
    Console.WriteLine(v < s);
  end Main;
end Program;
