//! case: s28_14_0018
//! doc: spec
//! section: 28.14
//! title: forming a method reference over a capture-free-register subscribe position is a compile error
//! rule: Register positions are stated at the callee, not marked in the
//!   parameter's function type, so forming a method reference over a
//!   capture-free-register position is a compile error, covering the
//!   shared-event subscribe positions.
//! cites: 28.14
//! expect: reject
//! diagnostic: method reference
module Conf;

public record Tick(Value: int);

public class Bus
  private static owned readonly _feed: SharedEventSource<Tick> := new();
  public static property Feed: ISharedEvent<Tick> => _feed;
end Bus;

public class Program
  public static method Main()
    var formed := Bus.Feed.Subscribe;
  end Main;
end Program;
