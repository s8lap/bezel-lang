//! case: l2_15_0018
//! doc: stdlib
//! section: 2.15
//! title: ReadDecimal rounds a long fraction once and raises SerializationException outside decimal range
//! rule: Its value is rounded to `decimal` as §9.3 rounds `decimal.Parse`'s input (`0.12345678901234567890123456785` reads as `0.1234567890123456789012345678`), and a number whose value, so rounded, lies outside `decimal`'s range (`79228162514264337593543950335.5`) raises `SerializationException` located at the number.
//! cites: 2.15, 9.3
//! expect: run
//! issue: 7340
//! output: {"V": 0.12345678901234567890123456785} -> OK 0.1234567890123456789012345678
//!   {"V": 79228162514264337593543950335.5} -> SER
//!   {"V": 79228162514264337593543950335} -> OK 79228162514264337593543950335

module Conf;

import System.Serialization;

record R(V: decimal): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show("{\"V\": 0.12345678901234567890123456785}");
    Show("{\"V\": 79228162514264337593543950335.5}");
    Show("{\"V\": 79228162514264337593543950335}");
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
