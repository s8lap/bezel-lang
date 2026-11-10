//! case: s31_2_1_0013
//! doc: spec
//! section: 31.2.1
//! title: a binding over an interface arm does not expose the interface's members
//! rule: An interface named in an arm is a filter: the arm takes exactly the
//!   throwables whose class implements it. It does not type a binding. … The
//!   interface's own members are reached by a type test inside the arm: `var r
//!   := ex as IRetryable;` gives a nullable borrow at the interface type and
//!   leaves `ex` an `Exception`.
//! cites: 31.2.1
//! expect: reject
//! diagnostic: does not type a binding
//! issue: 8415
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
      var r: IRetryable => Console.WriteLine(r.ShouldRetry())
    end;
  end Attempt;
end Worker;
