module Tools.Wc;

public class Program
  public static method Main()
    var lines := 0;
    var words := 0;
    var chars := 0;
    var line := Console.ReadLine();
    while line <> null do
      lines++;
      chars += line.Length + 1;
      for var w in line.Split(' ') do
        if w.Length > 0 then
          words++;
        end;
      end;
      line := Console.ReadLine();
    end;
    Console.WriteLine($"{lines} {words} {chars}");
  end Main;
end Program;
