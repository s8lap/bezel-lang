//! case: s27_8_0013
//! doc: spec
//! section: 27.8
//! title: an alias-typed nullable carrier narrowed by an is T test takes a with
//! rule: "A type alias is a pure synonym — it does not create a new type"
//!   (§27.8, v0.340); a `with` "result has the same static type as the
//!   source" (§9.5, v0.340); and when a test succeeds the carrier "is
//!   flow-narrowed to `T` within the controlled block" (§12.7, v0.340). So
//!   an alias-typed `P?` carrier narrowed by an `is P` test has source type
//!   `P` in the block, and the `with` result is a `P`.
//! cites: 27.8, 9.5, 12.7
//! expect: run
//! output: 44
//!   a
module Conf;

public record P(Sku: string, Stock: int);
type PAlias := P;

public class Program
  public static method Main()
    var d: Dictionary<string, P> := new Dictionary<string, P>();
    d["a"] := new P("a", 5);
    var c: PAlias? := d.GetOrNull("a");
    if c is P then
      var r: P := c with(Stock := 44);
      Console.WriteLine(r.Stock);
      Console.WriteLine(r.Sku);
    end;
  end Main;
end Program;
