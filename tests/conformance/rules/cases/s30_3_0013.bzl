//! case: s30_3_0013
//! doc: spec
//! section: 30.3
//! title: Result on a TaskCompletionSource task completed with SetResult returns the value
//! rule: `Wait()` and `Result` are synchronous escape hatches for code running outside any domain — in practice, the synchronous portion of program startup: a non-`async` `Main`, and code it calls directly on the main thread. There, they block the calling thread until the task completes
//! cites: 30.5
//! expect: run
//! output: 3
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
    using var tcs := new TaskCompletionSource<int>(); tcs.SetResult(3); var x := tcs.Task.Result; Console.WriteLine(x.ToString());
    return 0;
  end Main;
end C;
