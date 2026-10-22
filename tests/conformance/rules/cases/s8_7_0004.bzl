//! case: s8_7_0004
//! doc: spec
//! section: 8.7
//! title: Dispose on standard-library classes is accepted, directly and through a null-conditional receiver
//! rule: "The compiler generates `IDisposable` on every non-static class." (§8.7, v0.340) `Mutex` and `FileStream` are classes, so `m.Dispose()` and `f?.Dispose()` over a null `FileStream?` compile and run.
//! cites: 28.2
//! expect: run
//! stdout: ok
module Conf;

import System.IO;
import System.Threading;

public class Program
  public static method Main()
    var m := new Mutex();
    m.Dispose();
    var f: FileStream? := null;
    f?.Dispose();
    Console.WriteLine("ok");
  end Main;
end Program;
