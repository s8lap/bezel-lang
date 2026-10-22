//! case: s7_4_0097
//! doc: spec
//! section: 7.4
//! title: a with over a local narrowed by is not null, an early exit or an else-return is non-null
//! rule: "The compiler tracks, per local variable and per parameter other than a `ref` parameter, whether its value is known non-null" and member access on a nullable value is a compile error "unless the compiler has narrowed it to non-null" (§7.4, v0.340); a `with` "result has the same static type as the source" (§9.5, v0.340), so each spelling of the non-null fact (§7.4: `x is not null`, `if x is null then return; end;`, `if x is not null then else return; end;`) narrows the surviving path.
//! cites: 9.5
//! expect: run
//! output: 9
//!   11
//!   13
module Conf;

public record P(Sku: string, Stock: int);
public record G<T>(V: T, N: int);

public class Program
  public static method Main()
    var d: Dictionary<string, P> := new Dictionary<string, P>();
    d["a"] := new P("a", 5);
    var p := d.GetOrNull("a");
    if p is not null then
      var r: P := p with(Stock := 9);
      Console.WriteLine(r.Stock);
    end;
    var p2 := d.GetOrNull("a");
    if p2 is null then
      return;
    end;
    var r2: P := p2 with(Stock := 11);
    Console.WriteLine(r2.Stock);
    var p3 := d.GetOrNull("a");
    if p3 is not null then else return; end;
    var r3: P := p3 with(Stock := 12);
    Console.WriteLine(r3.Stock + r3.Sku.Length);
  end Main;
end Program;
