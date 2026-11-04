//! case: l2_15_0020
//! doc: stdlib
//! section: 2.15
//! title: ReadInt raises SerializationException outside the range of long
//! rule: a number read by `ReadInt` whose value lies outside `long`'s range, or by `ReadUInt` outside `ulong`'s (`9223372036854775808` and `-1` respectively), raises `SerializationException` located at the number
//! cites: 2.15
//! expect: run
//! issue: 7340
//! output: {"V": 9223372036854775808} -> SER
//!   {"V": -9223372036854775809} -> SER
//!   {"V": -9223372036854775808} -> OK -9223372036854775808

module Conf;

import System.Serialization;

record R(V: long): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show("{\"V\": 9223372036854775808}");
    Show("{\"V\": -9223372036854775809}");
    Show("{\"V\": -9223372036854775808}");
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
