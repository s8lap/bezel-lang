module Plugins;

public interface ICommand
  readonly property Name: string;
  method Run(args: Array<string>): int;
end ICommand;

public class Echo: ICommand
  public property Name: string => "echo";
  public method Run(args: Array<string>): int
    Console.WriteLine(String.Join(" ", args));
    return 0;
  end Run;
end Echo;

public class CountArgs: ICommand
  public property Name: string => "count";
  public method Run(args: Array<string>): int
    Console.WriteLine(args.Count);
    return 0;
  end Run;
end CountArgs;

public class Registry
  private owned _commands: OwningDictionary<string, ICommand> := new OwningDictionary<string, ICommand>();

  public method Register(owned cmd: ICommand)
    _commands[cmd.Name] := cmd;
  end Register;

  public method Dispatch(line: string): int
    var parts := line.Split(' ');
    var cmd := _commands.GetOrNull(parts[0]);
    if cmd = null then
      Console.WriteLine($"unknown command: {parts[0]}");
      return 127;
    end;
    return cmd.Run(parts[1..]);
  end Dispatch;

  public method Names(): owned Array<string>
    var names := _commands.Keys.ToArray();
    names.Sort();
    return names;
  end Names;
end Registry;

public class Program
  public static method Main()
    using var reg := new Registry();
    reg.Register(new Echo());
    reg.Register(new CountArgs());
    var lines: Array<string> := ["echo hello world", "count a b c", "nope x"];
    for var l in lines do
      reg.Dispatch(l);
    end;
    Console.WriteLine($"registered: {String.Join(", ", reg.Names())}");
  end Main;
end Program;
