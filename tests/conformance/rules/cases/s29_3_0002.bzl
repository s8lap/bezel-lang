//! case: s29_3_0002
//! doc: spec
//! section: 29.3
//! title: yield break terminates iteration early
//! rule: `yield break;` terminates iteration.
//! cites: 29.3
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method TakeWhilePositive(numbers: IEnumerable<int>): IEnumerable<int>
    for var n in numbers do
      if n <= 0 then
        yield break;
      end;
      yield return n;
    end;
  end TakeWhilePositive;

  public static method Main()
    var nums: Array<int> := [1, 2, -1, 5];
    var total := 0;
    for var n in TakeWhilePositive(nums) do
      total := total + n;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
