//! case: s7_4_0096
//! doc: spec
//! section: 7.4
//! title: a with over a local narrowed by a <> null test is the non-null record type
//! rule: "The compiler tracks, per local variable and per parameter other than a `ref` parameter, whether its value is known non-null" and member access on a nullable value is a compile error "unless the compiler has narrowed it to non-null" (§7.4, v0.340); a `with` "result has the same static type as the source" (§9.5, v0.340), so a source narrowed non-null by `p <> null` yields a non-null `P`, which `var r: P` accepts.
//! cites: 9.5
//! expect: run
//! output: 4
//!   7
module Conf;

public record P(Sku: string, Stock: int);
public record G<T>(V: T, N: int);

public class Program
  public static method Main()
    var d: Dictionary<string, P> := new Dictionary<string, P>();
    d["a"] := new P("a", 5);
    var p := d.GetOrNull("a");
    if p <> null then
      var r: P := p with(Stock := p.Stock - 1);
      d[p.Sku] := p with(Stock := 7);
      Console.WriteLine(r.Stock);
    end;
    Console.WriteLine(d["a"].Stock);
  end Main;
end Program;
