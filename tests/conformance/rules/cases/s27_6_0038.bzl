//! case: s27_6_0038
//! doc: spec
//! section: 27.6
//! title: a method reference to Dispose over a Task local is a compile error at its formation
//! rule: A method reference naming `Dispose` or `DisposeAsync` — bound (`d.Dispose` over a class- or
//!   interface-typed receiver, formed inside a closure or outside one) or type-qualified
//!   (`D.Dispose`, `IDisposable.Dispose`) — is a compile error at the formation, whatever the
//!   receiver.
//! cites: 28.6, 30.3
//! expect: reject
//! diagnostic: method reference
module Conf;

public class Program
  public static method Main(): int
    using var t := Task.FromResult(1);
    var f := t.Dispose;
    return 0;
  end Main;
end Program;
