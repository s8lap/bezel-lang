//! case: s6_5_0003
//! doc: spec
//! section: 6.5
//! title: bits shifted beyond the left operand's width are discarded silently and the result never faults
//! rule: Within the domain, bits shifted beyond the left operand's width are
//!   discarded silently — that is the operation, exactly as & discards bits —
//!   and the result never faults.
//! cites: 6.5
//! expect: run
//! stdout: 32
module Conf;

public class Program
  public static method Main()
    var b: byte := 200;
    var r := b << 2;
    Console.WriteLine(r);
  end Main;
end Program;
