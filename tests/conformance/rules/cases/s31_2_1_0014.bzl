//! case: s31_2_1_0014
//! doc: spec
//! section: 31.2.1
//! title: inside an interface arm, as reaches the interface and leaves the Exception surface
//! rule: The interface's own members are reached by a type test inside the arm:
//!   `var r := ex as IRetryable;` gives a nullable borrow at the interface type
//!   and leaves `ex` an `Exception`.
//! cites: 31.2.1, 12.7
//! expect: run
//! output: True
//!   server busy
//! issue: 8415
module Conf;

public interface IRetryable
  method ShouldRetry(): bool;
end IRetryable;

public class Busy(message: string): Exception(message), IRetryable
  public method ShouldRetry(): bool => true;
end Busy;

public class Program
  public static method Main()
    try
      throw new Busy("server busy");
    catch
      var ex: IRetryable => do
        var r := ex as IRetryable;
        Console.WriteLine(r!.ShouldRetry());
        Console.WriteLine(ex.Message);
      end
    end;
  end Main;
end Program;
