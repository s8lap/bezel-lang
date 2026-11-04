//! case: s9_8_0025
//! doc: spec
//! section: 9.8
//! title: a float component rejects a finite value whose float conversion is not finite
//! rule: Wherever a synthesized `Deserialize` reads a `float` value (a
//!   component, a nullable component's value, an element, a dictionary's key or
//!   value), it reads `d.ReadFloat()` and converts the `double` as §5.2's
//!   `float(x)`; a finite value whose conversion is not finite raises
//!   `SerializationException`, the component being unable to hold the number
//!   the input carries (a JSON `3.5e38` read into a `float` component raises).
//! cites: 9.8, 5.2
//! expect: run
//! output: SER
//!   ok 1.5
module Conf;

import System.Serialization;

public record Reading(V: float): ISerializable, IDeserializable;

public class Program
  public static method Main()
    Show("{\"V\": 3.5e38}");
    Show("{\"V\": 1.5}");
  end Main;

  private static method Show(json: string)
    try
      using var d := new JsonDeserializer(json);
      var r := Reading.Deserialize(d);
      Console.WriteLine($"ok {r.V}");
    catch
      SerializationException => Console.WriteLine("SER")
    end;
  end Show;
end Program;
