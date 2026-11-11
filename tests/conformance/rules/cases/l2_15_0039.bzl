//! case: l2_15_0039
//! doc: stdlib
//! section: 2.15
//! title: malformed JSON raises SerializationException, which an IFailure arm passes
//! rule: A class whose condition can come from the program itself, or from data
//!   whose supplier the library cannot see, is not marked, and makes no
//!   statement: text that does not parse and a document that does not
//!   deserialize may be a user's typing or the program's own constant
//!   (`FormatException`, `SerializationException`); an argument, the state of
//!   an object the program holds and the program's arithmetic are the program's
//!   (`ArgumentException`, `InvalidOperationException`, `KeyNotFoundException`,
//!   `OverflowException`).
//! cites: 2.15, 1
//! expect: run
//! stdout: malformed input passed
//! issue: 8414
module Conf;

import System.Serialization;

public record Wrapper(V: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    try
      try
        _ := Json.Parse<Wrapper>("not json");
      catch
        IFailure => Console.WriteLine("taken as a failure")
      end;
    catch
      SerializationException => Console.WriteLine("malformed input passed")
    end;
  end Main;
end Program;
