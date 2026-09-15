//! case: s28_14_0010
//! doc: spec
//! section: 28.14
//! title: a captured local declared after the using-var bounding object is rejected
//! rule: Every captured local source must be declared before L in the same
//!   or an enclosing scope; a source declared after L fails the
//!   declared-before test.
//! cites: 28.14
//! expect: reject
//! diagnostic: before
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
    await using var scope := new TaskScope();
    var w := new Widget();
    scope.Spawn(lambda() => w.PingAsync());
    await scope.Join();
  end Main;
end Program;
