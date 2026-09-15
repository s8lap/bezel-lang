//! case: s2_3_0006
//! doc: spec
//! section: 2.3
//! title: naming Ptr<T> requires importing System.Interop
//! rule: `System.Interop` — `Ptr<T>` and the `Interop` intrinsics — is
//!   deliberately not auto-imported, so *naming* `Ptr<T>` or `Interop`
//!   requires the `import System.Interop;` line.
//! cites: 2.3
//! expect: reject
//! diagnostic: Ptr
module Conf;

public class Program
  public static method Main()
    var p := Ptr<byte>.Zero;
  end Main;
end Program;
