//! case: s29_7_0003
//! doc: spec
//! section: 29.7
//! title: a second Dispose() on a partially-consumed iterator performs no second disposal
//! rule: The state machine runs the pending cleanup actions active at the
//!   point of suspension and transitions to a completed state — the machine's
//!   own bookkeeping, not a post-disposal guarantee to the consumer. A second
//!   `Dispose()` **is** observable and is a no-op: it is the single member
//!   §28.2's disposal contract keeps callable. The no-op is the runtime
//!   contract only: the second call is also a use of the binding and draws
//!   §28.2's use-after-dispose warning, which a `run` case tolerates (the
//!   corpus has no runs-and-warns tier).
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
