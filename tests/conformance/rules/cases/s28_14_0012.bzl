//! case: s28_14_0012
//! doc: spec
//! section: 28.14
//! title: a captured local source may not be explicitly disposed while the bounding object lives
//! rule: Until L's scope exits, the captured local sources are frozen — they
//!   may not be moved from, reassigned, or disposed.
//! cites: 28.14
//! expect: reject
//! diagnostic: dispose
module Conf;

import System.Threading;

public class Widget
  public constructor()
  end constructor;

  public async method PingAsync(): Task
    await Task.Delay(0);
  end PingAsync;
end Widget;

public class Program
  public static async method Main(): Task
    var w := new Widget();
    await using var scope := new TaskScope();
    scope.Spawn(lambda() => w.PingAsync());
    w.Dispose();
    await scope.Join();
  end Main;
end Program;
