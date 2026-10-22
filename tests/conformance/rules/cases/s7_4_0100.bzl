//! case: s7_4_0100
//! doc: spec
//! section: 7.4
//! title: a with over a narrowed generic record is the non-null generic record type
//! rule: "The compiler tracks, per local variable and per parameter other than a `ref` parameter, whether its value is known non-null" and member access on a nullable value is a compile error "unless the compiler has narrowed it to non-null" (§7.4, v0.340); a `with` "result has the same static type as the source" (§9.5, v0.340), so the rule is independent of the record being generic: a narrowed `G<string>?` and `G<int>?` yield non-null `G<string>` and `G<int>`.
//! cites: 9.5, 9.1
//! expect: run
//! output: 2
//!   0
//!   8
module Conf;

public record P(Sku: string, Stock: int);
public record G<T>(V: T, N: int);

public class Program
  static method Bump(g: G<string>?): G<string>
    if g = null then return new G<string>("z", 0); end;
    return g with(N := g.N + 1);
  end Bump;

  static method BumpI(g: G<int>?): int
    if g = null then return 0; end;
    var r: G<int> := g with(V := g.V + 5);
    return r.V + r.N;
  end BumpI;

  public static method Main()
    Console.WriteLine(Bump(new G<string>("a", 1)).N);
    Console.WriteLine(Bump(null).N);
    Console.WriteLine(BumpI(new G<int>(1, 2)));
  end Main;
end Program;
