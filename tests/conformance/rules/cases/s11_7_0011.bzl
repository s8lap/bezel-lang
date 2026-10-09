//! case: s11_7_0011
//! doc: spec
//! section: 11.7
//! title: a static owned readonly field on a static class is permitted
//! rule: Static `owned` fields are permitted: `static owned readonly` fields
//!   and `static readonly` owning-inline-array fields participate in §22.13's
//!   program-exit disposal exactly as on any other type — that mechanism is
//!   runtime-driven and independent of instance `Dispose()`.
//! cites: 11.7, 22.13, 28.5
//! expect: run
//! stdout: root
module Conf;

public class Handle
  private _tag: string;

  public constructor(tag: string)
    _tag := tag;
  end constructor;

  public property Tag: string => _tag;
end Handle;

public static class Registry
  public static owned readonly Root: Handle := new Handle("root");
end Registry;

public class Program
  public static method Main()
    Console.WriteLine(Registry.Root.Tag);
  end Main;
end Program;
