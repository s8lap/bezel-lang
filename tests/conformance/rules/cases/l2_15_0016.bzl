//! case: l2_15_0016
//! doc: stdlib
//! section: 2.15
//! title: the JSON deserializer rejects a number with an incomplete exponent, fraction or integer part
//! rule: an optional fraction — a `.` and one or more digits — and an optional exponent — `e` or `E`, an optional sign, and one or more digits
//! cites: 2.15
//! expect: run
//! issue: 7340
//! output: {"V": 1e} -> SER
//!   {"V": 1e+} -> SER
//!   {"V": 1e-} -> SER
//!   {"V": -} -> SER
//!   {"V": -.5} -> SER
//!   {"V": 01.5} -> SER
//!   {"V": 1.2.3} -> SER
//!   {"V": 1.5e1} -> OK 15
//!   {"V": 2E1} -> OK 20

module Conf;

import System.Serialization;

record R(V: double): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show("{\"V\": 1e}");
    Show("{\"V\": 1e+}");
    Show("{\"V\": 1e-}");
    Show("{\"V\": -}");
    Show("{\"V\": -.5}");
    Show("{\"V\": 01.5}");
    Show("{\"V\": 1.2.3}");
    Show("{\"V\": 1.5e1}");
    Show("{\"V\": 2E1}");
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
