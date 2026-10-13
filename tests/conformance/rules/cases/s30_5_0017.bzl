//! case: s30_5_0017
//! doc: spec
//! section: 30.5
//! title: a Task of a class disposed before it settles disposes the result its frame later produces
//! rule: Disposing a `Task`-family value of any kind — `Task`, `Task<T>`,
//!   `OwningTask<T>` — before it has settled does not stop its frame (§30.1):
//!   the `Dispose()` call attaches a **continuation** that runs at the
//!   terminal state and disposes there whatever the frame delivers into the
//!   task — a Pattern B result or a kind-1 Pattern A result
//! cites: 30.5, 30.1
//! expect: run
//! output: disposed
//!   closed
//!   done
//! issue: 5831
module Conf;
import System.Threading;

public class Res
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
    Demo.Done.SetResult();
  end destructor;
end Res;

public class Demo
  public static owned readonly Done: TaskCompletionSource := new TaskCompletionSource();

  public static async method MakeAsync(): Task<Res>
    await Task.Delay(50);
    return new Res();
  end MakeAsync;

  public static async method Main(): Task
    do
      using var t := MakeAsync();
    end;
    Console.WriteLine("disposed");
    await Demo.Done.Task;
    Console.WriteLine("done");
  end Main;
end Demo;
