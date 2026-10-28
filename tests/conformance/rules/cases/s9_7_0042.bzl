//! case: s9_7_0042
//! doc: spec
//! section: 9.7
//! title: a record may not implement IAsyncDisposable
//! rule: Language spec §9.7 (v0.345): "Records cannot have destructors, `owned` fields, or
//!   implement `IDisposable` — directly or through an interface extending it, `IAsyncDisposable`
//!   among them."
//! cites: 9.7, 28.12
//! expect: reject
//! diagnostic: IAsyncDisposable
module Conf;

public record R: IAsyncDisposable
  public _x: int := 0;
end R;

public class Program
  public static method Main()
    Console.WriteLine("hi");
  end Main;
end Program;
