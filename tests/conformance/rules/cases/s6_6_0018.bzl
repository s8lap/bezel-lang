//! case: s6_6_0018
//! doc: spec
//! section: 6.6
//! title: a compound assignment selects a user operator whose parameter is an interface
//! rule: "For every operator above except `??=`, the statement `x op= e` is equivalent to `x := x op
//!   e`" ([§6.6]). "records and enums by declaration" ([§5.1]): the record `Tag` conforms to `IA`
//!   by its declaration, so it converts to the `IA` parameter.
//! cites: 6.6, 5.1
//! expect: run
//! stdout: 1001
//! issue: 6717
module Conf;

public interface IA
end IA;

public record V(X: int)
  public static method operator +(v: V, p: IA): V => new V(v.X + 1000);
end V;

public record Tag(N: int): IA;

public class Program
  public static method Main()
    var v := new V(1);
    var t := new Tag(5);
    v += t;
    Console.WriteLine(v.X);
  end Main;
end Program;
