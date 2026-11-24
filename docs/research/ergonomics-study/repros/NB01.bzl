// bug: NB1
// title: `IFailure` is an unresolved type name
// issue: #8414; #8442
// rests-on: spec §31.4, stdlib §1, codegen reference §6, Guide 2 §4 (v0.359): `IFailure` marker and the `var f: IFailure =>` arm
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: run
// stdout: bad
module P;
public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;
public class Program
  public static method Main()
    try
      throw new ConfigError("bad");
    catch
      var f: IFailure => Console.WriteLine(f.Message)
    end;
  end Main;
end Program;
