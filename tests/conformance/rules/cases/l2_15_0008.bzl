//! case: l2_15_0008
//! doc: stdlib
//! section: 2.15
//! title: the JSON deserializer raises SerializationException on every malformed number
//! rule: **Errors.** Malformed input, type mismatches (a `ReadInt` when the next
//!   token is a string), and depth-limit violations raise `SerializationException`
//!   (declared in `System.Serialization`).
//! cites: 2.15, 9.2
//! expect: run
//! issue: 7340
//! output: {"V": nope} -> SER
//!   {"V": "str"} -> SER
//!   {"V": } -> SER
//!   {"V": +.5} -> SER
//!   {"V": .5} -> SER
//!   {"V": 1.5} -> OK 1.5

module Conf;

import System.Serialization;

record R(V: double): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show("{\"V\": nope}");
    Show("{\"V\": \"str\"}");
    Show("{\"V\": }");
    Show("{\"V\": +.5}");
    Show("{\"V\": .5}");
    Show("{\"V\": 1.5}");
  end Main;

  private static method Show(json: string)
    try
      using var d := new JsonDeserializer(json);
      var v := R.Deserialize(d);
      Console.WriteLine(json + " -> OK " + v.V.ToString());
    catch
      var e: SerializationException => do Console.WriteLine(json + " -> SER"); end,
      var e: FormatException => do Console.WriteLine(json + " -> FMT"); end
    end;
  end Show;
end Program;
