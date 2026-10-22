//! case: s7_4_0106
//! doc: spec
//! section: 7.4
//! title: a with is not narrowed after a call when the local is mutably captured by a lambda
//! rule: "The compiler tracks, per local variable and per parameter other than a `ref` parameter, whether its value is known non-null" and member access on a nullable value is a compile error "unless the compiler has narrowed it to non-null" (§7.4, v0.340); a `with` "result has the same static type as the source" (§9.5, v0.340), so a mutably captured local loses its fact at the first operation that can transfer control to user code (§7.4, v0.340): the call `Id()` follows the test, and the lambda assigns `p`.
//! cites: 9.5
//! expect: reject
//! diagnostic: the initializer for `r` is nullable but the target type is non-nullable
module Conf;

public record P(Sku: string, Stock: int);
public record G<T>(V: T, N: int);

public class Program
  static method Id(): int => 1;

  public static method Main()
    var d: Dictionary<string, P> := new Dictionary<string, P>();
    var p := d.GetOrNull("a");
    var f := lambda() => do p := null; end;
    if p = null then return; end;
    var n := Id();
    var r: P := p with(Stock := 1);
  end Main;
end Program;
