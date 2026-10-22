//! case: s7_4_0102
//! doc: spec
//! section: 7.4
//! title: a with over a nullable local that was never null-tested keeps the nullable type
//! rule: "The compiler tracks, per local variable and per parameter other than a `ref` parameter, whether its value is known non-null" and member access on a nullable value is a compile error "unless the compiler has narrowed it to non-null" (§7.4, v0.340); a `with` "result has the same static type as the source" (§9.5, v0.340), so with no test the source stays `P?`, so the result is `P?` and `var r: P` is rejected.
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
    var r: P := p with(Stock := 1);
  end Main;
end Program;
