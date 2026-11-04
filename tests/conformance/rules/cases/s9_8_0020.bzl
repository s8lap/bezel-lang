//! case: s9_8_0020
//! doc: spec
//! section: 9.8
//! title: an unknown field is skipped when the deserializer ignores unknown fields, an error by default
//! rule: An **unknown** field (present in the input, matching no declared
//!   component) is the one case governed by *operation* policy rather than the
//!   synthesized code: it is an error by default and is skipped when the
//!   deserializer is configured to ignore unknown fields (stdlib §2.15);
//! cites: 9.8, stdlib 2.15
//! expect: run
//! output: ok 1 2
//!   SER
//! notes: the skipped value is nested, so SkipValue must consume its whole
//!   object/array structure for the read to reach "Y".
module Conf;

import System.Serialization;

public record Point(X: int, Y: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var json := "{\"X\": 1, \"Z\": {\"q\": [1, 2]}, \"Y\": 2}";
    try
      using var d := new JsonDeserializer(json, new JsonOptions(false, 2, false, true, 64));
      var p := Point.Deserialize(d);
      Console.WriteLine($"ok {p.X} {p.Y}");
    catch
      SerializationException => Console.WriteLine("SER")
    end;
    try
      using var d := new JsonDeserializer(json);
      var p := Point.Deserialize(d);
      Console.WriteLine($"ok {p.X} {p.Y}");
    catch
      SerializationException => Console.WriteLine("SER")
    end;
  end Main;
end Program;
