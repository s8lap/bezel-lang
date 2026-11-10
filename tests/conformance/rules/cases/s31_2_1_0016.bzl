//! case: s31_2_1_0016
//! doc: spec
//! section: 31.2.1
//! title: the guard form retypes the binding to the interface
//! rule: The guard form `var ex: Exception when ex is IRetryable` instead retypes
//!   the binding for the arm (§12.7), which then sees the interface's members
//!   and no longer the `Exception` surface.
//! cites: 31.2.1, 12.7
//! expect: run
//! stdout: True
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
      var ex: Exception when ex is IRetryable => Console.WriteLine(ex.ShouldRetry())
    end;
  end Main;
end Program;
