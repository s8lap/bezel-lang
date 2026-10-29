//! case: s28_12_0034
//! doc: spec
//! section: 28.12
//! title: an enum may not list IAsyncDisposable in its conformance list
//! rule: Language spec §28.12 (v0.350): "only a class implements it — a record, enum or choice
//!   that lists it, or lists an interface extending it, is a compile error at the declaration, as
//!   no value kind implements `IDisposable`".
//! cites: 28.12, 8.7
//! expect: reject
//! diagnostic: IAsyncDisposable
//! issue: 7354
//! notes: the IAsyncDisposable spelling of s28_12_0030. The enum also lacks the DisposeAsync that
//!   §10.5 requires of an enum implementing an interface, so a second ground exists at the same
//!   declaration; the diagnostic fragment is what separates the two when the runner reports them.
//!   Harvested from the R36 reviewer probe issue7354_enum.
module Conf;

public enum E: IAsyncDisposable
  A, B;
end E;

public class Program
  public static method Main()
    Console.WriteLine("hi");
  end Main;
end Program;
