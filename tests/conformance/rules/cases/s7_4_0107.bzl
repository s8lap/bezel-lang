//! case: s7_4_0107
//! doc: spec
//! section: 7.4
//! title: a with in the else branch of a <> null test is not narrowed
//! rule: "The compiler tracks, per local variable and per parameter other than a `ref` parameter, whether its value is known non-null" and member access on a nullable value is a compile error "unless the compiler has narrowed it to non-null" (§7.4, v0.340); a `with` "result has the same static type as the source" (§9.5, v0.340), so a test narrows only "the true branch" for `<>`; in the `else` branch the local is known null, never narrowed non-null.
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
    if p <> null then
      Console.WriteLine("x");
    else
      var r: P := p with(Stock := 1);
    end;
  end Main;
end Program;
