//! case: s7_4_0108
//! doc: spec
//! section: 7.4
//! title: a with over a parameter narrowed by an is T test is the non-nullable record type
//! rule: The compiler tracks "per local variable and per parameter other than
//!   a `ref` parameter, whether its value is known non-null" (§7.4, v0.340),
//!   and when a test succeeds the carrier "is flow-narrowed to `T` within
//!   the controlled block" (§12.7, v0.340); a `with` "result has the same
//!   static type as the source" (§9.5, v0.340). So a `P?` parameter narrowed
//!   by an `is P` test has source type `P` in the block, and the `with`
//!   result is a `P`.
//! cites: 7.4, 9.5, 12.7
//! expect: run
//! output: 44
//!   alpha
module Conf;

public record P(Sku: string, Stock: int);

public class Program
  public static method Bump(p: P?): P
    if p is P then
      var r: P := p with(Stock := 44);
      return r;
    end;
    return new P("none", 0);
  end Bump;

  public static method Main()
    var r := Bump(new P("alpha", 5));
    Console.WriteLine(r.Stock);
    Console.WriteLine(r.Sku);
  end Main;
end Program;
