//! case: s31_4_0018
//! doc: spec
//! section: 31.4
//! title: an interface may extend IFailure and an exception class implement it
//! rule: An interface may extend `IFailure`; both restrictions then bind its
//!   implementers.
//! cites: 31.4
//! expect: accept
//! issue: 8414
module Conf;

public interface ITransient: IFailure
end ITransient;

public class Timeout: Exception, ITransient
end Timeout;

public class Probe
  public method Wait()
    try
      throw new Timeout();
    catch
      IFailure => Console.WriteLine("failure")
    end;
  end Wait;
end Probe;
