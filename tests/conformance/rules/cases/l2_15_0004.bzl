//! case: l2_15_0004
//! doc: stdlib
//! section: 2.15
//! title: a JSON-raised SerializationException carries a null Offset
//! rule: SerializationException locates the fault through Offset (binary)
//!   or Line/Column (JSON); the other format's properties are null on each
//!   instance.
//! cites: 2.15
//! expect: run
//! stdout: True
module Conf;

import System.Serialization;

public record Wrapper(V: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    try
      _ := Json.Parse<Wrapper>("not json");
    catch
      var ex: SerializationException => Console.WriteLine(ex.Offset = null)
    end;
  end Main;
end Program;
