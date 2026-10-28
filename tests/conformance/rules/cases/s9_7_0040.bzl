//! case: s9_7_0040
//! doc: spec
//! section: 9.7
//! title: a record may not implement IDisposable
//! rule: Language spec §9.7 (v0.345): "Records cannot have destructors, `owned` fields, or
//!   implement `IDisposable` — directly or through an interface extending it, `IAsyncDisposable`
//!   among them."
//! cites: 9.7
//! expect: reject
//! diagnostic: IDisposable
module Conf;

public record R: IDisposable
  public _x: int := 0;
end R;

public class Program
  public static method Main()
    Console.WriteLine("hi");
  end Main;
end Program;
