//! case: s30_6_0005
//! doc: spec
//! section: 30.6
//! title: the block-bodied reader async lambda forwarding through a using var task
//! rule: `var reader: (string) -> Task<string> := async lambda(path) => do
//!   using var task := File.ReadAllTextAsync(path); return await task; end;`
//!   — `Task<T>` for borrowing or value-typed results.
//! cites: 30.6, 30.5
//! expect: run
//! stdout: hi!
module Conf;

public class Fetcher
  public static async method FetchAsync(n: string): Task<string>
    await Task.Delay(0);
    return n + "!";
  end FetchAsync;
end Fetcher;

public class Program
  public static async method Main(): Task
    var reader: (string) -> Task<string> := async lambda(path) => do
      using var task := Fetcher.FetchAsync(path);
      return await task;
    end;
    using var t := reader("hi");
    var s := await t;
    Console.WriteLine(s);
  end Main;
end Program;
