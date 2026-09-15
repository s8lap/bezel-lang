//! case: s28_14_0013
//! doc: spec
//! section: 28.14
//! title: after the bounding local's scope exits the captured source is unfrozen
//! rule: After L's scope exits the sources are unfrozen, because L is
//!   disposed at that scope end — bounding L in a do ... end block is the
//!   sanctioned way to transfer a captured source out afterwards.
//! cites: 28.14
//! expect: run
//! stdout: done
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
    do
      await using var scope := new TaskScope();
      scope.Spawn(lambda() => w.PingAsync());
      await scope.Join();
    end;
    w.Dispose();
    Console.WriteLine("done");
  end Main;
end Program;
