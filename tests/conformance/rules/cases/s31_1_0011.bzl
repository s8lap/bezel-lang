//! case: s31_1_0011
//! doc: spec
//! section: 31.1
//! title: a closure over the caught exception confined to the arm is admitted
//! rule: A closure over the catch-arm binding is confined to the arm —
//!   admitted, however many lambdas nest inside it — whenever its value stays
//!   inside the arm: bound to an arm-local and invoked there, nothing over the
//!   binding ever leaves.
//! cites: 31.1, 28.3
//! expect: run
//! stdout: 8
module Conf;

public class Program
  public static method Use(ex: Exception): int => ex.Message.Length;

  public static method Main()
    var total := 0;
    try
      throw new ArgumentException("boom");
    catch
      var ex: Exception => do
        var mk := lambda(): () -> int => do return lambda() => Use(ex); end;
        var g := mk();
        total := g() + g();
      end,
    end try;
    Console.WriteLine(total);
  end Main;
end Program;
