//! case: l2_15_0019
//! doc: stdlib
//! section: 2.15
//! title: ReadInt raises SerializationException on a number with a fraction or an exponent whatever its value
//! rule: `ReadInt` and `ReadUInt` read only such a number, so one with a fraction or an exponent (`5.0`, `1e3`, `5.9`) is a type mismatch and raises `SerializationException` located at the number, whatever its value
//! cites: 2.15
//! expect: run
//! issue: 7340
//! output: {"V": 5.0} -> SER
//!   {"V": 1e3} -> SER
//!   {"V": 5.9} -> SER
//!   {"V": 5} -> OK 5

module Conf;

import System.Serialization;

record R(V: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show("{\"V\": 5.0}");
    Show("{\"V\": 1e3}");
    Show("{\"V\": 5.9}");
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
