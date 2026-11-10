module Errors.ParseSum;

public class Program
  public static method Main()
    var lines: Array<string> := ["a=1", "b=x", "c=3", "d", "e=-4"];
    var sum := 0;
    var good := 0;
    var bad := 0;
    for var i in 0..<lines.Count do
      var line := lines[i];
      var eq := line.IndexOf('=');
      if eq < 0 then
        Console.WriteLine($"line {i + 1}: missing '='");
        bad++;
        continue;
      end;
      var text := line.Substring(eq + 1);
      if int.TryParse(text, out var v) then
        sum += v;
        good++;
      else
        Console.WriteLine($"line {i + 1}: bad number '{text}'");
        bad++;
      end;
    end;
    Console.WriteLine($"sum={sum}");
    Console.WriteLine($"good={good} bad={bad}");
  end Main;
end Program;
