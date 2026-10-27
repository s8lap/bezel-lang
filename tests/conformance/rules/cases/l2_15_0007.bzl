//! case: l2_15_0007
//! doc: stdlib
//! section: 2.15
//! title: the JSON deserializer raises SerializationException on a fragment that is not a JSON number
//! rule: `ReadFloat` reads a JSON number of any length, its value rounded once to
//!   `double` as [§9.2](#sec-double-and-float) rounds `double.Parse`'s input; a
//!   number whose value, so rounded, is not finite (`1e999`, `-1e999`) raises
//!   `SerializationException` located at the number, so a document never reads
//!   back a non-finite value `WriteFloat` could not have written.
//! cites: 2.15
//! expect: run
//! issue: 7340
//! stdout: SER

module Conf;

import System.Serialization;

record R(V: double): ISerializable, IDeserializable;

public class Program
  public static method Main()
    try
      using var d := new JsonDeserializer("{\"V\": .5}");
      _ := R.Deserialize(d);
      Console.WriteLine("no raise");
    catch
      SerializationException => Console.WriteLine("SER");
    end;
  end Main;
end Program;
