//! case: s9_5_0008
//! doc: spec
//! section: 9.5
//! title: a with over an is-narrowed nullable carrier is the non-nullable record type
//! rule: "The result has the same static type as the source" (§9.5, v0.340);
//!   and when a test succeeds and the tested expression is one of §7.4's
//!   narrowing carriers, "the carrier is flow-narrowed to `T` within the
//!   controlled block" (§12.7, v0.340). So a `P?` carrier narrowed by an
//!   `is P` test has source type `P` inside the block, the `with` result has
//!   type `P`, and a `P` target accepts it; the replaced field takes the
//!   named value and the other fields keep the source's values (§9.5).
//! cites: 9.5, 12.7, 7.4
//! expect: run
//! output: 44
module Conf;

public record P(Sku: string, Stock: int);

public class Program
  public static method Main()
    var d: Dictionary<string, P> := new Dictionary<string, P>();
    d["a"] := new P("a", 5);
    var c := d.GetOrNull("a");
    if c is P then
      var r: P := c with(Stock := 44);
      Console.WriteLine(r.Stock);
    end;
  end Main;
end Program;
