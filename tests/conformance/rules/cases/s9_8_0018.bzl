//! case: s9_8_0018
//! doc: spec
//! section: 9.8
//! title: synthesized Deserialize dispatches by field name, whatever the input order
//! rule: Field order in the input need not match declaration order — dispatch
//!   is by field name.
//! cites: 9.8
//! expect: run
//! output: 1 2
//!   1 2
module Conf;

import System.Serialization;

public record Point(X: int, Y: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    using var d1 := new JsonDeserializer("{\"X\": 1, \"Y\": 2}");
    var a := Point.Deserialize(d1);
    Console.WriteLine($"{a.X} {a.Y}");
    using var d2 := new JsonDeserializer("{\"Y\": 2, \"X\": 1}");
    var b := Point.Deserialize(d2);
    Console.WriteLine($"{b.X} {b.Y}");
  end Main;
end Program;
