//! case: l2_15_0001
//! doc: stdlib
//! section: 2.15
//! title: Json.ToText / Json.Parse round-trip a value-type record
//! rule: Json.ToText<T> / Json.Parse<T> are defined as the corresponding
//!   agent sequence, producing and consuming the type's synthesized JSON
//!   encoding.
//! cites: 2.15, 2.14
//! expect: run
//! stdout: 3
module Conf;

import System.Serialization;

public record Point(X: int, Y: int): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var p := new Point(3, 4);
    var text := Json.ToText(p);
    var back := Json.Parse<Point>(text);
    Console.WriteLine(back.X);
  end Main;
end Program;
