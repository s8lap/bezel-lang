//! case: s6_6_0017
//! doc: spec
//! section: 6.6
//! title: a compound assignment selects a user operator whose parameter is Object
//! rule: "For every operator above except `??=`, the statement `x op= e` is equivalent to `x := x op
//!   e`" ([§6.6]). "reference types (classes including the heap arrays, interfaces, `string`,
//!   function types) directly" ([§5.1]): the `StringBuilder` operand converts to the `Object`
//!   parameter.
//! cites: 6.6, 5.1
//! expect: run
//! stdout: 1001
//! issue: 6688
module Conf;

public record V(X: int)
  public static method operator +(v: V, p: Object): V => new V(v.X + 1000);
end V;

public class Program
  public static method Main()
    var v := new V(1);
    using var r := new StringBuilder();
    v += r;
    Console.WriteLine(v.X);
  end Main;
end Program;
