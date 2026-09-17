//! case: s5_4_0006
//! doc: spec
//! section: 5.4
//! title: a widening cast from a byte-backed enum to int cannot fail
//! rule: int(p) is well-typed on every enum: for byte-backed Priority it is
//!   int(byte(p)) and cannot fail — the cast I(e) converts e to U and then
//!   applies §5.2's integer conversion I(u), so a widening never fails.
//! cites: 5.4, 5.2, 10.3
//! expect: run
//! stdout: 3
module Conf;

public enum Priority: byte
  Low := 1, Medium := 2, High := 3;
end Priority;

public class Program
  public static method Main()
    var p: Priority := Priority.High;
    Console.WriteLine(int(p));
  end Main;
end Program;
