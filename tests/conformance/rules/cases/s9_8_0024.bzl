//! case: s9_8_0024
//! doc: spec
//! section: 9.8
//! title: an integer component narrower than 64 bits rejects an out-of-range value
//! rule: Wherever it reads a value of an integer type narrower than 64 bits, a
//!   dictionary's key or value included, it reads `d.ReadInt()` for `sbyte`,
//!   `short` and `int` and `d.ReadUInt()` for `byte`, `ushort` and `uint`; a
//!   value outside the type's range raises `SerializationException` for the
//!   same reason, and is never truncated to the type's width (a JSON
//!   `5000000000` read into an `int` component, or `300` into a `byte`
//!   component, raises).
//! cites: 9.8, stdlib 2.15
//! expect: run
//! output: SER
//!   SER
//!   ok -7 255
module Conf;

import System.Serialization;

public record Sample(I: int, B: byte): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show("{\"I\": 5000000000, \"B\": 1}");
    Show("{\"I\": 1, \"B\": 300}");
    Show("{\"I\": -7, \"B\": 255}");
  end Main;

  private static method Show(json: string)
    try
      using var d := new JsonDeserializer(json);
      var s := Sample.Deserialize(d);
      Console.WriteLine($"ok {s.I} {s.B}");
    catch
      SerializationException => Console.WriteLine("SER")
    end;
  end Show;
end Program;
