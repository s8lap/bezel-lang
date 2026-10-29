//! case: s28_12_0033
//! doc: spec
//! section: 28.12
//! title: a choice may not list IAsyncDisposable in its conformance list
//! rule: Language spec §28.12 (v0.350): "only a class implements it — a record, enum or choice
//!   that lists it, or lists an interface extending it, is a compile error at the declaration, as
//!   no value kind implements `IDisposable`".
//! cites: 28.12, 8.7
//! expect: reject
//! diagnostic: IAsyncDisposable
//! issue: 7354
//! notes: the IAsyncDisposable spelling of s28_12_0031 (IDisposable). Harvested from the R36
//!   reviewer probe issue7354_choice; its `A,` line is normalized to s28_12_0031's `A, B;`.
module Conf;

public choice C: IAsyncDisposable
  A, B;
end C;

public class Program
  public static method Main()
    Console.WriteLine("hi");
  end Main;
end Program;
