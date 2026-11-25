//! case: s9_8_9102
//! doc: spec
//! section: 9.8
//! title: a record with a `[2]Shape` inline-array component of serializable choices round-trips
//!   through JSON
//! rule: A component type is **admissible as a serialized component** when it is
//! cites: 9.8, 2.15
//! expect: run
//! output: 1
//! issue: 9048
//! notes: Pins #9048 (pick 026db0dab): the JSON choice reader did not take the element separator,
//!   so an array of value-like choices serialized but failed to deserialize past its first
//!   element. An inline array `[N]T` whose element is an admissible component is itself
//!   admissible (9.8) and is a value type, so a record may hold it.
module Conf;

import System.Serialization;

public choice Shape: ISerializable, IDeserializable
  Circle(r: double),
  Rect(w: double, h: double),
  None;
end Shape;

public record Pair(A: [2]Shape): ISerializable, IDeserializable;

public class Program
  public static method Main(): int
    const p := new Pair([Shape.Circle(1.0), Shape.Rect(2.0, 3.0)]);
    using var s := new JsonSerializer();
    p.Serialize(s);
    using var d := new JsonDeserializer(s.ToString());
    const p2 := Pair.Deserialize(d);
    var sum := 0.0;
    for var i in 0..<2 do
      sum := sum + (case p2.A[i] of Circle(var r) => r, Rect(var w, var h) => w + h, None => 0.0);
    end;
    Console.WriteLine((if sum = 6.0 then 1 else 0));
    return 0;
  end Main;
end Program;
