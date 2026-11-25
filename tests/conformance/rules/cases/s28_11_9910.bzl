//! case: s28_11_9910
//! doc: spec
//! section: 28.11
//! title: A DictionaryEntry captured behind a never-taken branch does not fault
//! rule: copy out (a runtime-managed reference the copy holds — a `DictionaryEntry<string, V>`'s `Key` —
//!   keeps
//! cites: 28.11, 28.9, 27.5
//! expect: run
//! stdout: 0
//! notes: Harvested from the #9252 review (m7_capture_behind_if_never_taken). The branch holding the
//!   capture never runs, so the closure is never formed; the program must print 0. The pre-fix
//!   compiler faulted (null cell load) on this shape. The 35-byte Key leak that remains on this path
//!   is the open #9348 and is not observable from stdout; this case asserts only that the program
//!   runs.
module Conf;

public class Program
  public static method Main(): int
    var t := 0;
    using var d := new Dictionary<string, int>();
    d.Add("xy", 9);
    var fe := d.First();
    if t > 100 then
      var g := lambda() => fe.Key.Length + fe.Value;
      t := t + g();
    end if;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;
