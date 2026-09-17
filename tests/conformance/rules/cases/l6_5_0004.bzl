//! case: l6_5_0004
//! doc: stdlib
//! section: 6.5
//! title: SharedEventSource.Subscribe rejects a capturing handler
//! rule: Every subscribe position of the pair admits only capture-free
//!   handlers — a non-capturing lambda or a static method reference.
//! cites: 6.5
//! expect: reject
//! diagnostic: capture
module Conf;

import System.Threading;

public static class Bus
  private static owned readonly _tick: SharedEventSource<int> := new();
  public static property Tick: ISharedEvent<int> => _tick;
end Bus;

public class Program
  public method Demo()
    var captured := 10;
    using var sub := Bus.Tick.Subscribe(lambda(n) => Console.WriteLine(n + captured));
  end Demo;
end Program;
