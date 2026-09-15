//! case: s2_3_0007
//! doc: spec
//! section: 2.3
//! title: importing System.Interop makes Ptr<T> nameable
//! rule: `System.Interop` — `Ptr<T>` and the `Interop` intrinsics — is
//!   deliberately not auto-imported, so *naming* `Ptr<T>` requires the
//!   `import System.Interop;` line; supplying it makes the name resolve.
//! cites: 2.3
//! expect: run
//! stdout: zero
module Conf;
import System.Interop;

public class Program
  public static method Main()
    var p := Ptr<byte>.Zero;
    if p = Ptr<byte>.Zero then
      Console.WriteLine("zero");
    end if;
  end Main;
end Program;
