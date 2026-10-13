//! case: s28_6_0034
//! doc: spec
//! section: 28.6
//! title: disposing a static owned field from its declaring type is rejected
//! rule: `Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//!   compile error. The borrow receivers: ... a `static owned` field read (a
//!   borrow of K2's always-live source, which only §22.13's exit pass disposes
//!   — K9.4)
//! cites: 28.6, 22.13, 28.3
//! expect: reject
//! diagnostic: static owned
//! issue: 5807
module Conf;

public class Resource
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Resource;

public class Registry
  private static owned readonly _m: Resource := new Resource();

  public static method Close()
    Registry._m.Dispose();
  end Close;
end Registry;
