//! case: l2_15_0039
//! doc: stdlib
//! section: 2.15
//! title: malformed JSON raises a failure an IFailure arm takes
//! rule: The failure types this document declares are … `SerializationException`
//!   (§2.15) … with the built-in `FormatException` they are the library's whole
//!   failure band.
//! cites: 2.15, 1
//! expect: run
//! stdout: True
//! issue: 8414
module Conf;

import System.Serialization;

public record Wrapper(V: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    try
      _ := Json.Parse<Wrapper>("not json");
    catch
      var f: IFailure => Console.WriteLine(f is SerializationException)
    end;
  end Main;
end Program;
