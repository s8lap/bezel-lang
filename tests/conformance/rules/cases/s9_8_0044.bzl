//! case: s9_8_0044
//! doc: spec
//! section: 9.8
//! title: choice Deserialize requires a leading declared $case discriminator
//! rule: The synthesized `Deserialize` reads the discriminator first, consumed
//!   by case dispatch before any component binding — … an undeclared
//!   discriminator string raises `SerializationException`, as does an object
//!   that does not lead with the discriminator (dispatch must select the case
//!   before component binding can begin);
//! cites: 9.8
//! expect: run
//! output: rect 2 3
//!   empty
//!   SER
//!   SER
module Conf;

import System.Serialization;

public choice Shape: ISerializable, IDeserializable
  Rect(w: int, h: int),
  Empty;
end Shape;

public class Program
  public static method Main()
    Show("{\"$case\": \"Rect\", \"w\": 2, \"h\": 3}");
    Show("{\"$case\": \"Empty\"}");
    Show("{\"w\": 2, \"$case\": \"Rect\", \"h\": 3}");
    Show("{\"$case\": \"Circle\"}");
  end Main;

  private static method Show(json: string)
    try
      using var d := new JsonDeserializer(json);
      var v := Shape.Deserialize(d);
      Console.WriteLine((case v of Rect(w, h) => $"rect {w} {h}", Empty => "empty"));
    catch
      SerializationException => Console.WriteLine("SER")
    end;
  end Show;
end Program;
