//! case: s5_1_0141
//! doc: spec
//! section: 5.1
//! title: a type-parameter value does not implicitly convert to a primitive
//! rule: The §5.1 conversion table lists the implicit conversions, and a type
//!   parameter is not one of its source rows; §14.2 gives a type parameter the
//!   surface its constraints establish — a generic body converts a
//!   type-parameter value only to `Object` and to the bounds its constraints
//!   guarantee, never to a primitive.
//! cites: 5.1, 14.2
//! expect: reject
//! diagnostic: type parameter
module Conf;

public class Program
  public static method G<T>(x: T): int
    var y: int := x;      // a `T` value is not an `int` — REJECT
    return y;
  end G;

  public static method Main(): int
    var b: byte := 200;
    Console.WriteLine(Program.G<byte>(b).ToString());
    return 0;
  end Main;
end Program;
