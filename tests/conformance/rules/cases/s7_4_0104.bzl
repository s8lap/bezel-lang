//! case: s7_4_0104
//! doc: spec
//! section: 7.4
//! title: a with is not narrowed after the local is reassigned from a nullable expression
//! rule: "The compiler tracks, per local variable and per parameter other than a `ref` parameter, whether its value is known non-null" and member access on a nullable value is a compile error "unless the compiler has narrowed it to non-null" (§7.4, v0.340); a `with` "result has the same static type as the source" (§9.5, v0.340), so reassignment "from a nullable-typed expression" invalidates the fact (§7.4, v0.340), so the `with` source is `P?` again.
//! cites: 9.5
//! expect: reject
//! diagnostic: the initializer for `r` is nullable but the target type is non-nullable
module Conf;

public record P(Sku: string, Stock: int);
public record G<T>(V: T, N: int);

public class Program
  public static method Main()
    var d: Dictionary<string, P> := new Dictionary<string, P>();
    var p := d.GetOrNull("a");
    if p = null then return; end;
    p := d.GetOrNull("b");
    var r: P := p with(Stock := 1);
  end Main;
end Program;
