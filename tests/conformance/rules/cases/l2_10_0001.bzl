//! case: l2_10_0001
//! doc: stdlib
//! section: 2.10
//! title: user code may not implement Dispose() directly
//! rule: Every class implicitly implements IDisposable; user code does not
//!   implement it directly -- the customization points are a destructor and
//!   owned fields.
//! expect: reject
//! diagnostic: dispose
module Conf;

public class Resource
  public constructor()
  end constructor;

  public method Dispose()
  end Dispose;
end Resource;
