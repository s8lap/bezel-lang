//! case: s31_2_1_0018
//! doc: spec
//! section: 31.2.1
//! title: a type-parameter arm bounded by Exception and IFailure is admitted
//! rule: The parameter must carry an `Exception`-derived class bound (`where TEx:
//!   Exception`, or a class deriving from it) so that every substitution is a
//!   throwable class … A type parameter used as an arm type keeps its class
//!   bound (below): `where TEx: Exception, IFailure`, never the interface
//!   alone.
//! cites: 31.2.1, 31.4
//! expect: accept
//! issue: 8415
module Conf;

public class Check
  public static method Fails<TEx>(body: () -> ()): bool
      where TEx: Exception, IFailure
    try
      body();
    catch
      TEx => return true;
    end;
    return false;
  end Fails;
end Check;
