//! case: l2_15_0037
//! doc: stdlib
//! section: 2.15
//! title: an ignored unknown field is skipped whole, nested arrays and objects
//!   included
//! rule: When set, the deserializer skips an unrecognized field's value (via
//!   `IDeserializer.SkipValue`, §2.14, which consumes the value including any
//!   nested object/array structure) and continues rather than raising.
//! cites: 2.15, 2.14
//! expect: run
//! output: OK 5
//!   OK 5
//!   OK 5
//!   SER
//! notes: The first three inputs run with IgnoreUnknownFields set: the unknown value
//!   is an object holding an array holding an object, then an array holding a
//!   string with JSON punctuation, then plain scalars on both sides of the
//!   known field. The fourth is the strict twin: the same kind of unknown field
//!   raises when the option is false.
module Conf;

import System.Serialization;

record R1(V: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Console.WriteLine(Skip("{\"X\": {\"a\": [1, 2, {\"b\": 3}]}, \"V\": 5}", true));
    Console.WriteLine(Skip("{\"V\": 5, \"X\": [[], {\"k\": \"x,]}\"}]}", true));
    Console.WriteLine(Skip("{\"A\": 1, \"V\": 5, \"B\": \"s\"}", true));
    Console.WriteLine(Skip("{\"X\": {\"a\": [1, 2]}, \"V\": 5}", false));
  end Main;

  private static method Skip(j: string, ignore: bool): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j, new JsonOptions(false, 2, false, ignore, 64));
      var v := R1.Deserialize(d);
      r := $"OK {v.V}";
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end Skip;
end Program;
