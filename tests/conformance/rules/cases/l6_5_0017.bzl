//! case: l6_5_0017
//! doc: stdlib
//! section: 6.5
//! title: the ISharedEvent face subscribes and receives a raise
//! rule: SharedEventSource<T> implements ISharedEvent<T>, whose Subscribe
//!   admits a capture-free handler and returns an owned Subscription; Raise is
//!   a synchronized snapshot-raise whose handlers run on the raising domain.
//! cites: 6.5
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    using var src := new SharedEventSource<int>();
    var e: ISharedEvent<int> := src;
    using var sub := e.Subscribe(lambda(n) => Console.WriteLine(n));
    src.Raise(5);
  end Main;
end Program;
