//! case: l2_15_9103
//! doc: stdlib
//! section: 2.15
//! title: a hand-driven JSON array of one choice reads that element
//! rule: On the read side `JsonDeserializer.BeginArray()` returns `-1`, so consumers use
//!   `HasMoreElements()`.
//! cites: 2.15, 9.8
//! expect: run
//! output: 1
//!   1
//! issue: 9048
//! notes: Pins #9048 (pick 026db0dab): the JSON choice reader did not take the element separator,
//!   so an array of value-like choices serialized but failed to deserialize past its first
//!   element. Spec-decided boundary control: only one element, so no separator is taken
//!   between elements; green on both arms and it catches a fix that takes a separator
//!   unconditionally.
module Conf;

import System.Serialization;

public choice Shape: ISerializable, IDeserializable
  Circle(r: double),
  Rect(w: double, h: double),
  None;
end Shape;

public class Program
  public static method Main(): int
    using var d := new JsonDeserializer("[{\"$case\":\"Circle\",\"r\":1}]");
    _ := d.BeginArray();
    var n := 0;
    var sum := 0.0;
    while d.HasMoreElements() do
      var sh := Shape.Deserialize(d);
      sum := sum + (case sh of Circle(var r) => r, Rect(var w, var h) => w + h, None => 0.0);
      n++;
    end;
    d.EndArray();
    Console.WriteLine(n);
    Console.WriteLine((if sum = 1.0 then 1 else 0));
    return 0;
  end Main;
end Program;
