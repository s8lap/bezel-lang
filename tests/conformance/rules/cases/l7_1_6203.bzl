//! case: l7_1_6203
//! doc: stdlib
//! section: 7.1
//! title: a sliced ReadOnlySpan writes only its window and a sliced Span
//!   reads only into its window
//! rule: The **synchronous** span overloads carry their range in the value
//!   and are the preferred forms for synchronous I/O
//! cites: 7.1
//! expect: run
//! stdout: 3 0 0 2 3 4 0
module Conf;

import System.IO;

public class Program
  static method Produce(path: string)
    using var src := new OwningBuffer<byte>(6, lambda(i) => byte(i + 1));
    using var w := new FileStream(path, FileMode.Create, FileAccess.Write);
    w.Write(src.AsReadOnlySpan(1, 3));
  end Produce;

  static method ReadBack(path: string): string
    using var dst := new OwningBuffer<byte>(6);
    using var r := new FileStream(path, FileMode.Open, FileAccess.Read);
    var total := 0;
    var n := r.Read(dst.AsSpan(2, 3));
    while n > 0 do
      total := total + n;
      n := r.Read(dst.AsSpan(2 + total, 3 - total));
    end;
    return $"{total} {dst[0]} {dst[1]} {dst[2]} {dst[3]} {dst[4]} {dst[5]}";
  end ReadBack;

  public static method Main()
    var path := "l7_1_6203.tmp";
    Produce(path);
    Console.WriteLine(ReadBack(path));
    File.Delete(path);
  end Main;
end Program;
