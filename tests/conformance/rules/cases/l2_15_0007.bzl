//! case: l2_15_0007
//! doc: stdlib
//! section: 2.15
//! title: the JSON deserializer raises SerializationException on a fragment that is not a JSON number
//! rule: **Errors.** Malformed input, type mismatches (a `ReadInt` when the
//!   next token is a string), and depth-limit violations raise
//!   `SerializationException` (declared in `System.Serialization`); a JSON
//!   number has the shape the input grammar of
//!   [§9.2](#sec-double-and-float) admits, and that grammar admits no bare
//!   point.
//! cites: 2.15, 9.2
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
