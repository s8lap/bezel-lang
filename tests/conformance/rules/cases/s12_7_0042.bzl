//! case: s12_7_0042
//! doc: spec
//! section: 12.7
//! title: an is not T test narrows the carrier to T in the else branch
//! rule: For an `is not T` test, "the variable is narrowed to `T` in the
//!   else branch" (§12.7, v0.340); a `with` "result has the same static type
//!   as the source" (§9.5, v0.340). So in the else branch of `c is not P` a
//!   `P?` carrier is narrowed to `P`, the `with` result is a `P`, and a `P`
//!   target accepts it.
//! cites: 12.7, 9.5
//! expect: run
//! output: 44
//!   a
module Conf;

public record P(Sku: string, Stock: int);

public class Program
  public static method Main()
    var d: Dictionary<string, P> := new Dictionary<string, P>();
    d["a"] := new P("a", 5);
    var c := d.GetOrNull("a");
    if c is not P then
      Console.WriteLine("null branch");
    else
      var r: P := c with(Stock := 44);
      Console.WriteLine(r.Stock);
      Console.WriteLine(r.Sku);
    end;
  end Main;
end Program;
