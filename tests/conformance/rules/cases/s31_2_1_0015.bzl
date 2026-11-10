//! case: s31_2_1_0015
//! doc: spec
//! section: 31.2.1
//! title: the guard form retypes the binding and loses Message
//! rule: The guard form `var ex: Exception when ex is IRetryable` instead retypes
//!   the binding for the arm (§12.7), which then sees the interface's members
//!   and no longer the `Exception` surface.
//! cites: 31.2.1, 12.7
//! expect: reject
//! diagnostic: no longer the exception surface
module Conf;

public interface IRetryable
  method ShouldRetry(): bool;
end IRetryable;

public class Busy(message: string): Exception(message), IRetryable
  public method ShouldRetry(): bool => true;
end Busy;

public class Worker
  public method Attempt()
    try
      throw new Busy("server busy");
    catch
      var ex: Exception when ex is IRetryable => Console.WriteLine(ex.Message)
    end;
  end Attempt;
end Worker;
