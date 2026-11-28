//! case: s30_3_3601
//! doc: spec
//! section: 30.3
//! title: Wait() inside a non-async lambda nested in an async method is not rejected at compile time
//! rule: The compiler rejects `Wait()` and `Result` where the innermost enclosing method or lambda body is
//!   `async` (there, `await` is strictly superior and always available). A non-`async` lambda nested in an
//!   `async` body is not such a body
//! cites: 30.3, 27.3
//! expect: accept
//! issue: 8076
//! notes: Contrast s30_3_0002, where the `Wait()` stands directly in the async body. The task is already
//!   completed, so the runtime guard does not fire either.
module Conf;

public class Program
  public static async method RunAsync(): Task
    var f := lambda() => do
      using var t := Task.FromResult(1);
      t.Wait();
    end;
    f();
    await Task.Delay(0);
  end RunAsync;
end Program;
