//! case: l2_18_0001
//! doc: stdlib
//! section: 2.18
//! title: integer Dn pads with leading zeros to n digits
//! rule: Integers admit D/Dn decimal, n = min digits, zero-padded.
//! cites: 2.18
//! expect: run
//! stdout: 0005
module Conf;

public class Program
  public static method Main()
    var n := 5;
    Console.WriteLine($"{n:D4}");
  end Main;
end Program;
