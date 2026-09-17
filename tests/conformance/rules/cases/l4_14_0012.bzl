//! case: l4_14_0012
//! doc: stdlib
//! section: 4.14
//! title: TryReadInt16LittleEndian returns false and leaves value default on a short span
//! rule: Read methods throw IndexOutOfRangeException if the span is shorter
//!   than the type being read; TryRead* variants return false instead of
//!   throwing and assign the out parameter to default(T) on the false path.
//! cites: 4.14
//! expect: run
//! stdout: False 0
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<byte>(1, lambda(i: int): byte => byte(7));
    var ro := buf.AsReadOnlySpan();
    var ok := ro.TryReadInt16LittleEndian(out var value);
    Console.WriteLine($"{ok} {value}");
  end Main;
end Program;
