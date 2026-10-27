//! case: s12_7_0043
//! doc: spec
//! section: 12.7
//! title: a copy-out of an is-narrowed carrier reads the payload
//! rule: "A narrowing converts nothing" (§12.7, v0.340) — it changes the
//!   static type the carrier is seen at, not the value it holds. So a copy
//!   out of a `P?` carrier narrowed to `P` inside the controlled block
//!   copies the payload: the copied record's fields are the source's.
//! cites: 12.7, 7.4
//! expect: run
//! output: 5
//!   a
module Conf;

public record P(Sku: string, Stock: int);

public class Program
  public static method Main()
    var d: Dictionary<string, P> := new Dictionary<string, P>();
    d["a"] := new P("a", 5);
    var c := d.GetOrNull("a");
    if c is P then
      var x: P := c;
      Console.WriteLine(x.Stock);
      Console.WriteLine(x.Sku);
    end;
  end Main;
end Program;
