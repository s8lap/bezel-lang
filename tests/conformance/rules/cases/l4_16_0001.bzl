//! case: l4_16_0001
//! doc: stdlib
//! section: 4.16
//! title: a buffer span's SequenceEqual takes an array span of the other backing
//! rule: `SequenceEqual` is declared on each span type for its own type and
//!   for the read-only span of each heap backing (§4.14, §4.15; §3.3 for
//!   `char`)
//! cites: 4.16, 4.14, 4.15
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
