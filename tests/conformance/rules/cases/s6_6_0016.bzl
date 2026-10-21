//! case: s6_6_0016
//! doc: spec
//! section: 6.6
//! title: a compound assignment selects a user operator whose parameter is a stdlib class
//! rule: "For every operator above except `??=`, the statement `x op= e` is equivalent to `x := x op
//!   e`" ([§6.6]). "For `a <op> b` on non-primitive operands, the candidate set is the union of the
//!   `operator <op>` overloads declared on the static types of both operands, including
//!   declarations inherited from their base classes" ([§25.2]): `v += r` is `v := v + r`, and it
//!   selects the `StringBuilder` overload.
//! cites: 6.6, 25.2
//! expect: run
//! stdout: 1001
//! issue: 6656
module Conf;

public record V(X: int)
  public static method operator +(v: V, p: StringBuilder): V => new V(v.X + 1000);
end V;

public class Program
  public static method Main()
    var v := new V(1);
    using var r := new StringBuilder();
    v += r;
    Console.WriteLine(v.X);
  end Main;
end Program;
