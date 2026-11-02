//! case: s26_5_0062
//! doc: spec
//! section: 26.5
//! title: an extension on Task<T> supplies a method name callable through . and ?.
//! rule: If the receiver's type declares no instance member of that name, the
//!   compiler searches the current module and all imported modules — which
//!   always includes the standard library's extension modules over
//!   always-available types ([§2.3](#sec-always-available-names)'s invariant)
//!   — for extension blocks on `x`'s type, base types, or implemented
//!   interfaces, considering only extension members visible at the call site
//!   ([§26.3](#sec-extension-blocks-restrictions)'s effective-visibility
//!   rule).
//! cites: 26.5, 30.3, 7.5
//! expect: run
//! output: 4
//!   5
module Conf;

extension Task<T>
  public method Wait2(k: int): int => k;
end extension;

public class Program
  public static method Main(): int
    using var t := Task.FromResult(2);
    Console.WriteLine(t.Wait2(4).ToString());
    var t2: Task<int>? := t;
    var r := t2?.Wait2(5);
    Console.WriteLine((r ?? 0).ToString());
    return 0;
  end Main;
end Program;
