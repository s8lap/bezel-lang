//! case: s17_8_0004
//! doc: spec
//! section: 17.8
//! title: a with over a null-forgiven source is the non-nullable record type
//! rule: "`x!` asserts that `x` is non-null. The result type is the
//!   non-nullable form of the expression's type" (§17.8, v0.340); a `with`
//!   "result has the same static type as the source" (§9.5, v0.340). So a
//!   `with` over a null-forgiven `P?` source has type `P`, and a `P` target
//!   accepts it.
//! cites: 17.8, 9.5
//! expect: run
//! output: 44
//!   a
module Conf;

public record P(Sku: string, Stock: int);

public class Program
  public static method Main()
    var c: P? := new P("a", 5);
    var r: P := c! with(Stock := 44);
    Console.WriteLine(r.Stock);
    Console.WriteLine(r.Sku);
  end Main;
end Program;
