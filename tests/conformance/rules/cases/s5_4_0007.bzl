//! case: s5_4_0007
//! doc: spec
//! section: 5.4
//! title: narrowing a long-backed enum value to int raises OverflowException out of range
//! rule: For long-backed HttpStatus, int(s) is int(long(s)) and raises
//!   OverflowException when a NON-CONSTANT value is outside int's range; a
//!   declared-constant operand outside it is a compile error at the cast site
//!   (v0.301). A local variable read is not a constant expression (§17.14).
//! cites: 5.4, 5.2, 10.3, 17.14
//! expect: run
//! stdout: overflow
module Conf;

public enum HttpStatus: long
  Ok := 200, Huge := 5000000000;
end HttpStatus;

public class Program
  public static method Main()
    try
      var s: HttpStatus := HttpStatus.Huge;
      var n := int(s);
      Console.WriteLine(n);
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
