//! case: s22_12_0017
//! doc: spec
//! section: 22.12
//! title: a capturing handler passed to SharedEventSource.Subscribe is rejected
//! rule: Every call resolving to a subscribe position declared on
//!   `ISharedEvent<T>`/`SharedEventSource<T>` admits only capture-free
//!   handlers: a non-capturing lambda or a static method reference.
//! cites: 22.12, 28.14
//! expect: reject
//! diagnostic: capture-free
module Conf;

public static class Bus
  private static owned readonly _e: SharedEventSource<int> := new();

  public static method Listen(threshold: int)
    using var _ := _e.Subscribe(lambda(x: int) => do
      Console.WriteLine(x + threshold);
    end);
  end Listen;
end Bus;
