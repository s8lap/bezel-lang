//! case: l7_1_6204
//! doc: stdlib
//! section: 7.1
//! title: a span Read at the end of the file returns 0
//! rule: and `0` **iff** the stream is at end
//! cites: 7.1
//! expect: run
//! stdout: 3 0
module Conf;

import System.IO;

public class Program
  static method Produce(path: string)
    using var src := new OwningBuffer<byte>(3, lambda(i) => byte(5));
    using var w := new FileStream(path, FileMode.Create, FileAccess.Write);
    w.Write(src.AsReadOnlySpan());
  end Produce;

  static method ReadBack(path: string): string
    using var dst := new OwningBuffer<byte>(8);
    using var r := new FileStream(path, FileMode.Open, FileAccess.Read);
    var total := 0;
    while total < 3 do
      var n := r.Read(dst.AsSpan(total, 3 - total));
      if n = 0 then break; end;
      total := total + n;
    end;
    var again := r.Read(dst.AsSpan(0, 3));
    return $"{total} {again}";
  end ReadBack;

  public static method Main()
    var path := "l7_1_6204.tmp";
    Produce(path);
    Console.WriteLine(ReadBack(path));
    File.Delete(path);
  end Main;
end Program;
