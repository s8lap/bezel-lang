//! case: l2_15_0006
//! doc: stdlib
//! section: 2.15
//! title: the JSON deserializer raises SerializationException on a number that rounds to a non-finite value
//! rule: a number whose value, so rounded, is not finite (`1e999`, `-1e999`) raises
//!   `SerializationException` located at the number, so a document never reads
//!   back a non-finite value `WriteFloat` could not have written.
//! cites: 2.15, 9.2
//! expect: run
//! stdout: SER

module Conf;

import System.Serialization;

record R(V: double): ISerializable, IDeserializable;

public class Program
  public static method Main()
    try
      using var d := new JsonDeserializer("{\"V\": 1e999}");
      _ := R.Deserialize(d);
      Console.WriteLine("no raise");
    catch
      SerializationException => Console.WriteLine("SER");
    end;
  end Main;
end Program;
