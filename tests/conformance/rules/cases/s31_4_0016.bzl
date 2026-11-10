//! case: s31_4_0016
//! doc: spec
//! section: 31.4
//! title: an Exception-derived class listing IFailure is admitted
//! rule: Only an `Exception`-derived class: a record, an enum, a choice, or a class
//!   outside the `Exception` family that lists `IFailure`, or an interface
//!   extending it, is a compile error at the declaration.
//! cites: 31.4
//! expect: accept
//! issue: 8414
module Conf;

public class Reading: Exception, IFailure
end Reading;

public class Probe
  public method Read()
    try
      throw new Reading();
    catch
      IFailure => Console.WriteLine("failure")
    end;
  end Read;
end Probe;
