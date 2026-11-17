//! case: l7_1_6205
//! doc: stdlib
//! section: 7.1
//! title: a span longer than the remaining file bounds the read by the file,
//!   not by the span
//! rule: `ReadAsync` has the same contract with the wait asynchronous; the
//!   span overload has it over its own `Length`.
//! cites: 7.1
//! expect: run
//! stdout: 3 5 5
module Conf;

import System.IO;

public class Program
  static method Produce(path: string)
    using var src := new OwningBuffer<byte>(3, lambda(i) => byte(5));
    using var w := new FileStream(path, FileMode.Create, FileAccess.Write);
    w.Write(src.AsReadOnlySpan());
  end Produce;

  static method ReadBack(path: string): string
    using var dst := new OwningBuffer<byte>(100);
    using var r := new FileStream(path, FileMode.Open, FileAccess.Read);
    var total := 0;
    var n := r.Read(dst.AsSpan());
    while n > 0 do
      total := total + n;
      n := r.Read(dst.AsSpan(total));
    end;
    return $"{total} {dst[0]} {dst[2]}";
  end ReadBack;

  public static method Main()
    var path := "l7_1_6205.tmp";
    Produce(path);
    Console.WriteLine(ReadBack(path));
    File.Delete(path);
  end Main;
end Program;
