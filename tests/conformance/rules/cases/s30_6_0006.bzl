//! case: s30_6_0006
//! doc: spec
//! section: 30.6
//! title: the block-bodied loader async lambda forwards an owned Result through its state machine
//! rule: `var loader: (string) -> OwningTask<Resource> := async lambda(path)
//!   => do return await LoadResourceAsync(path); end;` — "Pattern B return;
//!   the lambda's state machine forwards the owned Resource through."
//! cites: 30.6, 30.5
//! expect: run
//! output: res
//!   res freed
module Conf;

public class Resource
  public _name: string;
  public constructor()
    _name := "res";
  end constructor;
  destructor
    Console.WriteLine("res freed");
  end destructor;
end Resource;

public class Loader
  public static async method LoadResourceAsync(p: string): OwningTask<Resource>
    await Task.Delay(0);
    return new Resource();
  end LoadResourceAsync;
end Loader;

public class Program
  public static async method Main(): Task
    var loader: (string) -> OwningTask<Resource> := async lambda(path) => do
      return await Loader.LoadResourceAsync(path);
    end;
    using var t := loader("a");
    using var r := await t;
    Console.WriteLine(r._name);
  end Main;
end Program;
