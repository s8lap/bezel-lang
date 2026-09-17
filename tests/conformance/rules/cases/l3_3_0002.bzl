//! case: l3_3_0002
//! doc: stdlib
//! section: 3.3
//! title: Slice returns the requested sub-range
//! rule: Slice(start, length) constructs a new ReadOnlyStringSpan over the
//!   requested range.
//! cites: 3.3
//! expect: run
//! stdout: world
module Conf;

public class Program
  public static method Main()
    var s := "hello world";
    var span := s.AsSpan();
    Console.WriteLine(span.Slice(6, 5).ToString());
  end Main;
end Program;
