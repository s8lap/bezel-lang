//! case: s7_4_0105
//! doc: spec
//! section: 7.4
//! title: a with over a mutable field is never narrowed
//! rule: "The compiler tracks, per local variable and per parameter other than a `ref` parameter, whether its value is known non-null" and member access on a nullable value is a compile error "unless the compiler has narrowed it to non-null" (§7.4, v0.340); a `with` "result has the same static type as the source" (§9.5, v0.340), so the compiler "never narrows mutable fields, properties, or `ref` parameters" (§7.4, v0.340), so the early-exit test on `Item` does not make `Item with(...)` non-null.
//! cites: 9.5
//! expect: reject
//! diagnostic: the returned value is nullable but the target type is non-nullable
module Conf;

public record P(Sku: string, Stock: int);
public record G<T>(V: T, N: int);

public class H
  public Item: P?;

  public method F(): P
    if Item = null then return new P("a", 1); end;
    return Item with(Stock := 1);
  end F;
end H;

public class Program
  public static method Main() end Main;
end Program;
