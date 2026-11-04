//! case: l2_15_0017
//! doc: stdlib
//! section: 2.15
//! title: ReadDecimal raises SerializationException on a JSON number with an exponent
//! rule: `ReadDecimal` reads only a JSON number with no exponent, of any length: one with an exponent (`1e3`, `1.5E-2`, `1e0`) is a type mismatch and raises `SerializationException` located at the number, whatever its value
//! cites: 2.15, 9.3
//! expect: run
//! issue: 7340
//! output: {"V": 1e3} -> SER
//!   {"V": 1.5E-2} -> SER
//!   {"V": 1e0} -> SER
//!   {"V": 1.5} -> OK 1.5

module Conf;

import System.Serialization;

record R(V: decimal): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show("{\"V\": 1e3}");
    Show("{\"V\": 1.5E-2}");
    Show("{\"V\": 1e0}");
    Show("{\"V\": 1.5}");
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
