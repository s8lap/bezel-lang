//! case: s7_4_0103
//! doc: spec
//! section: 7.4
//! title: a with is not narrowed when the null-exit sits on only one path into it
//! rule: "The compiler tracks, per local variable and per parameter other than a `ref` parameter, whether its value is known non-null" and member access on a nullable value is a compile error "unless the compiler has narrowed it to non-null" (§7.4, v0.340); a `with` "result has the same static type as the source" (§9.5, v0.340), so "a fact holds at a point only when it holds on every path that reaches it" (§7.4, v0.340): the early exit is under an `if args.Count > 0`, so `p` can still be null at the `with`.
//! cites: 9.5
//! expect: reject
//! diagnostic: the initializer for `r` is nullable but the target type is non-nullable
module Conf;

public record P(Sku: string, Stock: int);
public record G<T>(V: T, N: int);

public class Program
  public static method Main(args: Array<string>)
    var d: Dictionary<string, P> := new Dictionary<string, P>();
    var p := d.GetOrNull("a");
    if args.Count > 0 then
      if p = null then return; end;
    end;
    var r: P := p with(Stock := 1);
  end Main;
end Program;
