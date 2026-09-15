//! case: s28_2_0010
//! doc: spec
//! section: 28.2
//! title: an interface may declare a member named Dispose
//! rule: Interfaces are unaffected by the class-and-record rejection of a
//!   member named `Dispose` — `IDisposable` declares `method Dispose();` and
//!   interfaces extend it.
//! cites: 28.2
//! expect: accept
//! issue: 4404
module Conf;

public interface ICloseable
  method Dispose();
  method Flush();
end ICloseable;
