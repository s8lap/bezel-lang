//! case: s28_3_0196
//! doc: spec
//! section: 28.3
//! title: an implementation iterates its inherited () -> V parameter's result as a borrow
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

public interface ISource<V>
  method Put(f: () -> V);
end ISource;

public class Drainer: ISource<IEnumerable<int>>
  public constructor()
  end constructor;

  public static method Nums(): IEnumerable<int>
    yield return 1;
  end Nums;

  public method Put(f: () -> IEnumerable<int>)
    var n := 0;
    for var x in f() do
      n := n + x;
    end;
  end Put;
end Drainer;
