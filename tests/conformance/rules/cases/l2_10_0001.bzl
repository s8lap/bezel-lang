//! case: l2_10_0001
//! doc: stdlib
//! section: 2.10
//! title: user code may not implement Dispose() directly
//! rule: Every class implicitly implements IDisposable; user code does not
//!   implement it directly -- a class or record declaring a member named
//!   Dispose, at any parameter list, is a compile error at that declaration.
//! cites: 2.10
//! expect: reject
//! diagnostic: dispose
module Conf;

public class Resource
  public constructor()
  end constructor;

  public method Dispose()
  end Dispose;
end Resource;
