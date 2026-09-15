//! case: s25_2_0010
//! doc: spec
//! section: 25.2
//! title: operands are matched in order, with no automatic commutation
//! rule: Parameters match operands in order — there is no automatic commutation
//!   (`4 - v` is not matched against `operator -(v: V, n: int)`).
//! cites: 25.2
//! expect: reject
//! diagnostic: operator
module Conf;

public record Scaled(N: int)
  public static method operator -(v: Scaled, n: int): Scaled =>
    new Scaled(v.N - n);
end Scaled;

public class Program
  public static method Main()
    var v := new Scaled(10);
    var r := 4 - v;
    Console.WriteLine(r.N);
  end Main;
end Program;
