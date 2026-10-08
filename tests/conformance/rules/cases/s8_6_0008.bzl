//! case: s8_6_0008
//! doc: spec
//! section: 8.6
//! title: rendering a span through its own ToString member call is admitted
//! rule: The remedies at a rejected conversion are a parameter or local declared
//!   at the value's own type or at `IReadOnlySpan<TElement>`, a class
//!   implementing the interface, or — for rendering — `x.ToString()` or an
//!   interpolation hole, which invoke a member on the value
//! cites: 8.6, 8.5, 9.7
//! expect: run
//! stdout: begin
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<int>(4);
    var s := buf.AsSpan();
    Console.WriteLine("begin");
    Console.WriteLine(s.ToString());
  end Main;
end Program;
