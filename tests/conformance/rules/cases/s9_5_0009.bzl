//! case: s9_5_0009
//! doc: spec
//! section: 9.5
//! title: a with over a plain non-nullable record source keeps the other fields' values
//! rule: "The `with` operation constructs the copy directly from the
//!   source's storage" and "All other fields keep their values from the
//!   source record" (§9.5, v0.340). So a `with` over a plain, non-nullable
//!   record source replaces only the named field — including a nested
//!   record field, which is copied whole and not unwrapped.
//! cites: 9.5
//! expect: run
//! output: 3
//!   y
//!   9
module Conf;

public record Inner(Sku: string, Stock: int);
public record Outer(Flag: byte, Value: Inner);

public class Program
  public static method Main()
    var i: Inner? := new Inner("x", 5);
    var o: Outer := new Outer(7, new Inner("y", 9));
    if i <> null then
      var r: Outer := o with(Flag := 3);
      Console.WriteLine(r.Flag);
      Console.WriteLine(r.Value.Sku);
      Console.WriteLine(r.Value.Stock);
    end;
  end Main;
end Program;
