//! case: l2_15_0012
//! doc: stdlib
//! section: 2.15
//! title: the JSON deserializer raises SerializationException on every non-JSON spelling of an int
//! rule: Any other spelling of a number — `.5`, `5.`, `1.e3`, `+5`, `007`, `1_000`, `0x1p3` — is malformed input
//! cites: 2.15
//! expect: run
//! issue: 7340
//! output: {"V": +5} -> SER
//!   {"V": 007} -> SER
//!   {"V": 1_000} -> SER
//!   {"V": 0x1p3} -> SER
//!   {"V": 5.} -> SER
//!   {"V": 5} -> OK 5

module Conf;

import System.Serialization;

record R(V: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show("{\"V\": +5}");
    Show("{\"V\": 007}");
    Show("{\"V\": 1_000}");
    Show("{\"V\": 0x1p3}");
    Show("{\"V\": 5.}");
    Show("{\"V\": 5}");
  end Main;

  private static method Show(json: string)
    try
      using var d := new JsonDeserializer(json);
      var v := R.Deserialize(d);
      Console.WriteLine(json + " -> OK " + v.V.ToString());
    catch
      SerializationException => Console.WriteLine(json + " -> SER");
    end;
  end Show;
end Program;
