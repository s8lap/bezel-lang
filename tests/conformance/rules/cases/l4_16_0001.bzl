//! case: l4_16_0001
//! doc: stdlib
//! section: 4.16
//! title: SequenceEqual on the interface admits cross-source comparison between span families
//! rule: SequenceEqual is on the interface, with parameter type
//!   IReadOnlySpan<TElement> -- this admits cross-source equality: a
//!   ReadOnlySpan<byte> can be compared against a ReadOnlyArraySpan<byte>.
//! cites: 4.16
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<byte>(2, lambda(i: int): byte => byte(i));
    using var xs: Array<byte> := [byte(0), byte(1)];
    var bufSpan := buf.AsReadOnlySpan();
    var arrSpan := xs.AsReadOnlySpan();
    Console.WriteLine(bufSpan.SequenceEqual(arrSpan));
  end Main;
end Program;
