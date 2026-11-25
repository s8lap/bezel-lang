//! case: s9_8_9101
//! doc: spec
//! section: 9.8
//! title: a class with an owned Array of serializable choices round-trips through JSON with every
//!   element
//! rule: A component type is **admissible as a serialized component** when it is
//! cites: 9.8, 2.15
//! expect: run
//! output: 3
//!   1
//! issue: 9048
//! notes: Pins #9048 (pick 026db0dab): the JSON choice reader did not take the element separator,
//!   so an array of value-like choices serialized but failed to deserialize past its first
//!   element. `Array<Shape>` of a value-like serializable choice is an admissible component
//!   (9.8, sequence collection row); the synthesized Deserialize must read each element.
module Conf;

import System.Serialization;

public choice Shape: ISerializable, IDeserializable
  Circle(r: double),
  Rect(w: double, h: double),
  None;
end Shape;

public class Bag(owned items: Array<Shape>): ISerializable, IDeserializable
  public owned Items: Array<Shape> := items;
end Bag;

public class Program
  public static method Main(): int
    using var b := new Bag(new Array<Shape>());
    b.Items.Add(Shape.Circle(1.0));
    b.Items.Add(Shape.Rect(2.0, 3.0));
    b.Items.Add(Shape.None);
    using var s := new JsonSerializer();
    b.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    using var b2 := Bag.Deserialize(d);
    Console.WriteLine(b2.Items.Count);
    var sum := 0.0;
    for var i in 0..<3 do
      sum := sum + (case b2.Items[i] of Circle(var r) => r, Rect(var w, var h) => w + h, None => 0.0);
    end;
    Console.WriteLine((if sum = 6.0 then 1 else 0));
    return 0;
  end Main;
end Program;
