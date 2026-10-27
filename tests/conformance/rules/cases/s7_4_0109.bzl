//! case: s7_4_0109
//! doc: spec
//! section: 7.4
//! title: a with over a generic record narrowed by an is T test is the non-nullable generic record type
//! rule: The compiler tracks "per local variable and per parameter other than
//!   a `ref` parameter, whether its value is known non-null" (§7.4, v0.340),
//!   and when a test succeeds the carrier "is flow-narrowed to `T` within
//!   the controlled block" (§12.7, v0.340); a `with` "result has the same
//!   static type as the source" (§9.5, v0.340). The rule is independent of
//!   the record being generic: a `Box<int>?` narrowed by an `is Box<int>`
//!   test yields a `Box<int>` with.
//! cites: 7.4, 9.5, 12.7, 9.1
//! expect: run
//! output: 44
module Conf;

public record Box<T>(Value: T);

public class Program
  public static method Main()
    var b: Box<int>? := new Box<int>(5);
    if b is Box<int> then
      var r: Box<int> := b with(Value := 44);
      Console.WriteLine(r.Value);
    end;
  end Main;
end Program;
