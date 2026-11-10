module Files.NumberLines;

import System.IO;

public class Program
  public static method Main()
    var dir := Path.Combine("/var/tmp/bezel-scratch/ergo", "t21tmp");
    Directory.CreateDirectory(dir);
    var input := Path.Combine(dir, "in.txt");
    var output := Path.Combine(dir, "out.txt");
    // r40: a collection literal passed straight to WriteAllLines(IEnumerable<string>) writes nothing (compiler bug).
    var lines: Array<string> := ["alpha", "beta", "gamma"];
    File.WriteAllLines(input, lines);
    var numbered := new Array<string>();
    var n := 1;
    for var line in File.ReadAllLines(input) do
      numbered.Add($"{n,3}: {line}");
      n++;
    end;
    File.WriteAllLines(output, numbered);
    Console.Write(File.ReadAllText(output));
    File.Delete(input);
    File.Delete(output);
    Directory.Delete(dir);
  end Main;
end Program;
