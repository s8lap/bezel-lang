//! case: s22_13_0009
//! doc: spec
//! section: 22.13
//! title: eager startup assignment of a Task cache avoids the check-then-store race
//! rule: Eager startup assignment of the task cache — assign the
//!   `Task<T>?` cache before any spawn or other first use; the
//!   check-then-store race never arises and the work runs exactly once.
//! cites: 22.13, 22.12
//! expect: run
//! output: computed
//!   42
module Conf;

public static class Loader
  private static _cfg: Task<int>? := null;

  public static method Preload()
    _cfg := Compute();
  end Preload;

  public static async method Compute(): Task<int>
    Console.WriteLine("computed");
    return 42;
  end Compute;

  public static async method GetValue(): Task<int>
    if _cfg = null then _cfg := Compute(); end;
    return await _cfg!;
  end GetValue;
end Loader;

public class Program
  public static async method Main(): Task
    Loader.Preload();
    Console.WriteLine(await Loader.GetValue());
  end Main;
end Program;
