//! case: s30_5_0018
//! doc: spec
//! section: 30.5
//! title: a kind-2 chain over an owned parameter formed after outer disposal is disposed by the continuation
//! rule: the `Dispose()` call attaches a **continuation** that runs at the
//!   terminal state and disposes there whatever the frame delivers into the
//!   task — ... the source task a kind-2 slot chains ... Every disposal the
//!   task's teardown would have performed at or after settlement is performed
//!   by the continuation instead, exactly once
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

  public static async method Pass(owned inner: Task<Res>): Task<Res>
    return await inner;
  end Pass;

  public static async method Main(): Task
    do
      using var t := Pass(MakeAsync());
    end;
    Console.WriteLine("disposed");
    await Demo.Done.Task;
    Console.WriteLine("done");
  end Main;
end Demo;
