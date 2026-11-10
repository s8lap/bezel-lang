module Files.Binary;

import System.IO;

public class Program
  public static method Main()
    const path := "/var/tmp/bezel-scratch/ergo/t22.bin";
    var values: Array<int> := [1, 256, -2, 1000000];
    do
      using var fs := new FileStream(path, FileMode.Create, FileAccess.Write);
      using var buf := new OwningBuffer<byte>(4);
      for var v in values do
        buf.AsSpan().WriteInt32LittleEndian(v);
        fs.Write(buf.AsReadOnlySpan());
      end;
    end;
    using var rs := new FileStream(path, FileMode.Open, FileAccess.Read);
    Console.WriteLine($"{rs.Length} bytes");
    using var chunk := new OwningBuffer<byte>(4);
    var sum := 0L;
    var parts := new Array<string>();
    while rs.Read(chunk.AsSpan()) = 4 do
      var v := chunk.AsReadOnlySpan().ReadInt32LittleEndian();
      sum += v;
      parts.Add(v.ToString());
    end;
    Console.WriteLine(String.Join(" ", parts));
    Console.WriteLine($"sum {sum}");
    File.Delete(path);
  end Main;
end Program;
