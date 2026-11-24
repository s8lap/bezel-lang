// bug: B12
// title: Owned trivial-dispose local in an `async` body is a hard error
// issue: #1772
// rests-on: spec §28.9 (no must-use diagnostic at a trivial-dispose type) and §28.3 (an implicit drop carries at most the must-use warning)
// source: data/compiler-bugs.md, verbatim below this header
// expect: quiet
// stdout: 0
module P;
public class Flaky
  private _calls: int := 0;
  public method Get(): int => _calls;
end Flaky;
public class Program
  public static async method Main(): Task
    var f := new Flaky();
    await Task.Delay(1);
    Console.WriteLine(f.Get());
  end Main;
end Program;
