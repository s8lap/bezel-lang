module Tools.WordCount;

import System.IO;

public class Program
  public static method Main(args: Array<string>): int
    var path := args[0];
    if not File.Exists(path) then
      Console.Error.WriteLine($"not found: {path}");
      return 1;
    end;
    var lines := File.ReadAllLines(path);        // owned Array<string>
    var words := 0;
    for var line in lines do
      words := words + line.Split(' ').Count;    // Split: owned Array<string>, empty segments kept
    end;
    Console.WriteLine($"{lines.Count} lines, {words} words");
    return 0;
  end Main;
end Program;
