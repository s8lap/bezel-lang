//! case: s7_4_0098
//! doc: spec
//! section: 7.4
//! title: a with over a narrowed parameter is the non-null record type in a return and in an argument
//! rule: "The compiler tracks, per local variable and per parameter other than a `ref` parameter, whether its value is known non-null" and member access on a nullable value is a compile error "unless the compiler has narrowed it to non-null" (§7.4, v0.340); a `with` "result has the same static type as the source" (§9.5, v0.340), so a parameter other than `ref` is tracked as a local is: after the early exit the `P?` parameter is non-null, so `p with(...)` is returned as `P` and passed to a `P` parameter.
//! cites: 9.5
//! expect: run
//! output: 3
//!   0
//!   21
//!   -1
module Conf;

public record P(Sku: string, Stock: int);
public record G<T>(V: T, N: int);

public class Program
  static method Take(x: P): int => x.Stock;

  static method Ret(p: P?): P
    if p = null then
      return new P("n", 0);
    end;
    return p with(Stock := 3);
  end Ret;

  static method Arg(p: P?): int
    if p = null then return -1; end;
    return Take(p with(Stock := 21));
  end Arg;

  public static method Main()
    Console.WriteLine(Ret(new P("a", 1)).Stock);
    Console.WriteLine(Ret(null).Stock);
    Console.WriteLine(Arg(new P("a", 1)));
    Console.WriteLine(Arg(null));
  end Main;
end Program;
