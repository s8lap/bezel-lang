//! case: s25_2_0022
//! doc: spec
//! section: 25.2
//! title: a relational operator with an interface parameter applies to an array operand
//! rule: "For `a <op> b` on non-primitive operands, the candidate set is the union of the `operator
//!   <op>` overloads declared on the static types of both operands, including declarations
//!   inherited from their base classes" ([§25.2]). "classes by declaration or inheritance"
//!   ([§5.1]): the `Array<int>` operand converts to the `IEnumerable<int>` parameter of `<`.
//! cites: 25.2, 5.1
//! expect: run
//! stdout: True
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
    var a: Array<int> := [1, 2];
    Console.WriteLine(v < a);
  end Main;
end Program;
