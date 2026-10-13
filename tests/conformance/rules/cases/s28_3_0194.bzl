//! case: s28_3_0194
//! doc: spec
//! section: 28.3
//! title: the same implementation invoking the parameter with a capture-free lambda is admitted
//! rule: A method implementing or overriding a member of a generic type ...
//!   whose **declaring** signature places a type parameter in a slot of a
//!   function-typed parameter ... reads that parameter, in its own body and at
//!   every call site, direct or dispatched, at the declaring member's
//!   pre-substitution type ... So over `interface ISink<V> method Take(f: (V)
//!   -> ()); method Put(f: () -> V); end ISink;`, `class Feeder: ISink<() ->
//!   int>` may not invoke its `f: (() -> int) -> ()` with a closure over its
//!   own `using var` local, nor pass `f` to a method declaring `k: (() -> int)
//!   -> ()` concretely, and `class Drainer: ISink<IEnumerable<int>>` iterates
//!   `f()` in its `Put(f: () -> IEnumerable<int>)` as a borrow of `f`'s set,
//!   disposing nothing, while `d.Put(lambda() => Nums())` over a yield-based
//!   `Nums` is rejected at the lambda's return path
//! cites: 28.3, 11.2
//! expect: accept
module Conf;

public interface ISink<V>
  method Take(f: (V) -> ());
end ISink;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public class Feeder: ISink<() -> int>
  public constructor()
  end constructor;

  public method Take(f: (() -> int) -> ())
    using var it := new Item();
    f(lambda() => 1);
  end Take;
end Feeder;
