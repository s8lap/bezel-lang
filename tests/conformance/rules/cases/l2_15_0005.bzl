//! case: l2_15_0005
//! doc: stdlib
//! section: 2.15
//! title: an unknown field raises SerializationException by default
//! rule: A field present in the input that matches no declared component
//!   is, by default (IgnoreUnknownFields = false), a SerializationException
//!   — the strict default.
//! cites: 2.15
//! expect: run
//! stdout: caught
module Conf;

import System.Serialization;

public record Wrapper(V: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    try
      _ := Json.Parse<Wrapper>("{\"V\":1,\"Extra\":2}");
    catch
      SerializationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
