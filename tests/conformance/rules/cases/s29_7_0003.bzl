//! case: s29_7_0003
//! doc: spec
//! section: 29.7
//! title: a second Dispose() on a partially-consumed iterator is a no-op
//! rule: When a consumer stops iterating early the iterator is disposed: the
//!   state machine runs the pending cleanup actions active at the point of
//!   suspension, transitions to a completed state, and a second `Dispose()` is
//!   a no-op. `Dispose()` on any class instance is idempotent — the first call
//!   performs the disposal, subsequent calls are no-ops (§28.2).
//! cites: 29.7, 28.2
//! expect: run
//! output: cleanup
//!   done
module Conf;

public class Resource
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("cleanup");
  end destructor;
end Resource;

public class Program
  public static method Gen(): IEnumerable<int>
    using var r := new Resource();
    yield return 1;
    yield return 2;
  end Gen;

  public static method Main()
    using var src := Gen();
    var e := src.GetEnumerator();
    _ := e.MoveNext();
    e.Dispose();
    e.Dispose();
    Console.WriteLine("done");
  end Main;
end Program;
