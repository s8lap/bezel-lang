//! case: s9_7_0043
//! doc: spec
//! section: 9.7
//! title: a nested record may not implement IDisposable either
//! rule: Language spec §9.7 (v0.345): "Records cannot have destructors, `owned` fields, or
//!   implement `IDisposable` — directly or through an interface extending it, `IAsyncDisposable`
//!   among them." The rule is stated of records as a kind, with no clause limiting it to
//!   module-level declarations, so a record declared inside a class is a record too.
//! cites: 9.7, 11.6
//! expect: reject
//! diagnostic: IDisposable
//! issue: 7490
//! notes: the compiler ADMITTED it until fix #7490 — `Resolver::checkRecordRestrictions`
//!   walked the module's children, so a nested record escaped every §9.6/§9.7 record
//!   restriction. The reach now covers the nested types of this module at any depth, and
//!   this case is rejected with "record `R` may not implement `IDisposable` (§9.7)".
module Conf;

public class Outer
  public record R: IDisposable
    public _x: int := 0;
  end R;
end Outer;

public class Program
  public static method Main()
    Console.WriteLine("hi");
  end Main;
end Program;
