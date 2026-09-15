//! case: l2_15_0003
//! doc: stdlib
//! section: 2.15
//! title: writing a non-finite float raises SerializationException
//! rule: WriteFloat produces a JSON number, with non-finite values (NaN,
//!   ±Infinity) raising SerializationException — JSON has no representation
//!   for them.
//! cites: 2.15
//! expect: run
//! stdout: caught
module Conf;

import System.Serialization;

public record Score(Value: double): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var s := new Score(double.NaN);
    try
      _ := Json.ToText(s);
    catch
      SerializationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
