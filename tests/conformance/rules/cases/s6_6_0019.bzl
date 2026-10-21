//! case: s6_6_0019
//! doc: spec
//! section: 6.6
//! title: a compound assignment with no applicable user operator is rejected
//! rule: "For every operator above except `??=`, the statement `x op= e` is equivalent to `x := x op
//!   e`" ([§6.6]). "nor an overload from the candidate set above applies is a compile error at the
//!   expression" ([§25.2]): no overload accepts a `StringBuilder` for `IA`.
//! cites: 6.6, 25.2
//! expect: reject
//! diagnostic: applicable operator
//! issue: 6656
module Conf;

public interface IA
end IA;

public record V(X: int)
  public static method operator +(v: V, p: IA): V => new V(v.X + 1000);
end V;

public class Program
  public static method Main()
    var v := new V(1);
    using var r := new StringBuilder();
    v += r;
    Console.WriteLine(v.X);
  end Main;
end Program;
