//! case: s31_2_1_0007
//! doc: spec
//! section: 31.2.1
//! title: an interface-typed catch arm matches an implementing exception
//! rule: An interface-typed arm remains admissible — no interface derives
//!   from `Exception`, but an `Exception`-derived class may implement one, so
//!   the arm matches exactly the throwables whose class implements the
//!   interface.
//! cites: 31.2.1
//! expect: run
//! stdout: True
module Conf;

public interface IRetryable
  method ShouldRetry(): bool;
end IRetryable;

public class RetryableException: Exception, IRetryable
  public constructor(message: string) base(message)
  end constructor;

  public method ShouldRetry(): bool => true;
end RetryableException;

public class Program
  public static method Main()
    try
      throw new RetryableException("boom");
    catch
      var ex: IRetryable => Console.WriteLine(ex.ShouldRetry().ToString()),
    end try;
  end Main;
end Program;
