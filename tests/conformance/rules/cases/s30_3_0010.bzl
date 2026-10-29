//! case: s30_3_0010
//! doc: spec
//! section: 30.3
//! title: Result on a completed Task<int> from a non-async Main blocks and returns the value
//! rule: `Wait()` and `Result` are synchronous escape hatches for code running outside any domain — in practice, the synchronous portion of program startup: a non-`async` `Main`, and code it calls directly on the main thread. There, they block the calling thread until the task completes
//! cites: 30.5
//! expect: run
//! output: 5
module Conf;
import System.Text;
import System.Threading;
class Holder
  public t: Task<int>? := null;
end Holder;
class C
  public static async method Comp(v: int): Task<int>
    await Task.Delay(1);
    return v;
  end Comp;
  public static async method CompS(v: string): OwningTask<StringBuilder>
    await Task.Delay(1);
    return new StringBuilder(v);
  end CompS;
  public static method Gen<X>(t: Task<X>): X
    return t.Result;
  end Gen;
  public static method Par(t: Task<int>): int
    return t.Result;
  end Par;

  public static method Main(): int
    var x := Task.FromResult(5).Result; Console.WriteLine(x.ToString());
    return 0;
  end Main;
end C;
