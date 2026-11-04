//! case: l2_15_0021
//! doc: stdlib
//! section: 2.15
//! title: ReadUInt raises SerializationException on a negative number and outside the range of ulong
//! rule: a number read by `ReadInt` whose value lies outside `long`'s range, or by `ReadUInt` outside `ulong`'s (`9223372036854775808` and `-1` respectively), raises `SerializationException` located at the number
//! cites: 2.15
//! expect: run
//! issue: 7340
//! output: {"V": -1} -> SER
//!   {"V": 18446744073709551616} -> SER
//!   {"V": 18446744073709551615} -> OK 18446744073709551615

module Conf;

import System.Serialization;

record R(V: ulong): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show("{\"V\": -1}");
    Show("{\"V\": 18446744073709551616}");
    Show("{\"V\": 18446744073709551615}");
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
