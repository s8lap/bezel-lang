//! case: l2_2_0005
//! doc: stdlib
//! section: 2.2
//! title: Fmod and IEEERemainder may disagree in sign on the same operands
//! rule: Fmod(x, y) returns a result with the same sign as x; IEEERemainder(x, y)
//!   may have the opposite sign from x -- unlike Fmod.
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var fmod := Math.Fmod(-5.0, 3.0);
    var ieee := Math.IEEERemainder(-5.0, 3.0);
    Console.WriteLine(fmod < 0.0 and ieee > 0.0);
  end Main;
end Program;
