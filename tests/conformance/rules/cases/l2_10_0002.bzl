//! case: l2_10_0002
//! doc: stdlib
//! section: 2.10
//! title: a destructor is the customization point for disposal, not Dispose()
//! rule: Every class implicitly implements IDisposable; user code does not
//!   implement it directly -- the customization points are a destructor and
//!   owned fields.
//! expect: accept
module Conf;

public class Resource
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("cleanup");
  end destructor;
end Resource;
