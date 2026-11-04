//! case: s9_8_0019
//! doc: spec
//! section: 9.8
//! title: a missing or duplicate field raises SerializationException even under a lenient deserializer
//! rule: A **missing** required component (one neither present nor
//!   `serialized optional`), a **duplicate** field name, a type mismatch, or a
//!   name not matching any enum constant raises `SerializationException`
//!   unconditionally — these are structural faults in the input regardless of
//!   policy.
//! cites: 9.8
//! expect: run
//! output: ok 1 2
//!   SER
//!   SER
//! notes: every read is under IgnoreUnknownFields = true (stdlib 2.15's
//!   JsonOptions), so neither fault can be excused as an unknown field.
module Conf;

import System.Serialization;

public record Point(X: int, Y: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show("{\"X\": 1, \"Y\": 2}");
    Show("{\"X\": 1}");
    Show("{\"X\": 1, \"X\": 3, \"Y\": 2}");
  end Main;

  private static method Show(json: string)
    try
      using var d := new JsonDeserializer(json, new JsonOptions(false, 2, false, true, 64));
      var p := Point.Deserialize(d);
      Console.WriteLine($"ok {p.X} {p.Y}");
    catch
      SerializationException => Console.WriteLine("SER")
    end;
  end Show;
end Program;
