//! case: s30_5_0032
//! doc: spec
//! section: 30.5
//! title: a scoped await-bind inside a loop body is disposed per iteration
//! rule: A bind inside a loop body pins to the loop *body*: per-iteration
//!   disposal, no accumulation across iterations.
//! cites: 30.5
//! expect: run
//! issue: 3048
//! output: use r1
//!   dispose r1
//!   use r2
//!   dispose r2
//!   end
module Conf;

public class Res
  private _n: string;
  public constructor(n: string) _n := n; end constructor;

  public method Name(): string => _n;

  destructor
    Console.WriteLine($"dispose {_n}");
  end destructor;
end Res;

public class Producer
  public static async method MakeAsync(n: string): Task<Res>
    await Task.Delay(0);
    return new Res(n);
  end MakeAsync;
end Producer;

public class Program
  public static async method Main(): Task
    for var i in 1..2 do
      var r := await Producer.MakeAsync($"r{i}");
      Console.WriteLine($"use {r.Name()}");
    end;
    Console.WriteLine("end");
  end Main;
end Program;
