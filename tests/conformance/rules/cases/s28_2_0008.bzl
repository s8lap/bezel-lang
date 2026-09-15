//! case: s28_2_0008
//! doc: spec
//! section: 28.2
//! title: a record member named Dispose is rejected
//! rule: Declaring a member named `Dispose` on a class or a **record** is a
//!   compile error at that declaration, whatever its parameter list; on a
//!   record the name promises a disposal contract the kind does not have
//!   (§9.7 — records do not implement `IDisposable`).
//! cites: 28.2, 9.7
//! expect: reject
//! diagnostic: dispose
//! issue: 4404
module Conf;

public record Setting
  public readonly property Mode: int;

  public constructor(mode: int)
    Mode := mode;
  end constructor;

  public method Dispose()
    Console.WriteLine("closing");
  end Dispose;
end Setting;
