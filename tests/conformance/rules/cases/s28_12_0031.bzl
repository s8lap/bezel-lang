//! case: s28_12_0031
//! doc: spec
//! section: 28.12
//! title: a choice may not list IDisposable in its conformance list
//! rule: Language spec §28.12 (v0.345): "only a class implements it — a record, enum or choice
//!   that lists it, or lists an interface extending it, is a compile error at the declaration, as
//!   no value kind implements `IDisposable`".
//! cites: 28.12, 8.7
//! expect: reject
//! diagnostic: IDisposable
module Conf;

public choice C: IDisposable
  A, B;
end C;

public class Program
  public static method Main()
    Console.WriteLine("hi");
  end Main;
end Program;
