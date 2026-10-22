//! case: s7_4_0099
//! doc: spec
//! section: 7.4
//! title: a with over a narrowed readonly field is the non-null record type
//! rule: "The compiler tracks, per local variable and per parameter other than a `ref` parameter, whether its value is known non-null" and member access on a nullable value is a compile error "unless the compiler has narrowed it to non-null" (§7.4, v0.340); a `with` "result has the same static type as the source" (§9.5, v0.340), so "A nullable `readonly` field — instance or `static` — participates in flow narrowing" (§7.4, v0.340), through `this._f` and through `v._f` for a tracked local `v`.
//! cites: 9.5
//! expect: run
//! output: 4
//!   100
//!   0
module Conf;

public record P(Sku: string, Stock: int);
public record G<T>(V: T, N: int);

public class Holder
  public readonly Cur: P?;

  public constructor(i: P?)
    Cur := i;
  end;

  public method Dec(): P
    if Cur = null then return new P("none", 0); end;
    return Cur with(Stock := Cur.Stock - 1);
  end Dec;
end Holder;

public class Program
  public static method Main()
    var h := new Holder(new P("abc", 5));
    Console.WriteLine(h.Dec().Stock);
    if h.Cur <> null then
      var r: P := h.Cur with(Stock := 100);
      Console.WriteLine(r.Stock);
    end;
    var h2 := new Holder(null);
    Console.WriteLine(h2.Dec().Stock);
  end Main;
end Program;
