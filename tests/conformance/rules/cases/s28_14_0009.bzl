//! case: s28_14_0009
//! doc: spec
//! section: 28.14
//! title: a captured local declared before the using-var bounding object is admitted
//! rule: A closure argument capturing locals is admitted iff the bounding
//!   object resolves to a using var local L where every captured local
//!   source is declared before L in the same or an enclosing scope.
//! cites: 28.14
//! expect: run
//! stdout: ping
module Conf;

import System.Threading;

public class Widget
  public constructor()
  end constructor;

  public async method PingAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("ping");
  end PingAsync;
end Widget;

public class Program
  public static async method Main(): Task
    var w := new Widget();
    await using var scope := new TaskScope();
    scope.Spawn(lambda() => w.PingAsync());
    await scope.Join();
  end Main;
end Program;
