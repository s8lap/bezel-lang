//! case: s16_3_0001
//! doc: spec
//! section: 16.3
//! title: a Range<double> is rejected — the element type is restricted to the integer types and char
//! rule: "`T` is restricted by the compiler to the integer types (`byte`, `sbyte`,
//!   `short`, `ushort`, `int`, `uint`, `long`, `ulong`) and `char`. Other
//!   substitutions are rejected — at the range expression or the `Range<T>`
//!   annotation... `float`, `double`, and `decimal` conform [to `ISteppable<T>`]
//!   and are not admitted."
//! cites: 16.3
//! expect: reject
//! diagnostic: restricted
module Conf;

public class Program
  public static method Main()
    var r: Range<double> := 1.0..2.0;
  end Main;
end Program;
