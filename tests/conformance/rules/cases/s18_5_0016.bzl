//! case: s18_5_0016
//! doc: spec
//! section: 18.5
//! title: a null hole of any nullable reference or value type renders the text null with no dispatch
//! rule: A hole expression may have a nullable type; when the value is `null`
//!   at render time, the hole renders as the four-character text `null`, and no
//!   `ToString()` dispatch occurs.
//! cites: 18.5, 8.5
//! expect: run
//! output: null|null|null|null|null|null|null
//!   null|null|null|null|null|null|null
module Conf;

public class Program
  public static method Main()
    var d0: Dictionary<int, int>? := null;
    var c0: Channel<int>? := null;
    var r0: Random? := null;
    var a0: Array<int>? := null;
    var st0: Stack<int>? := null;
    var m0: Mutex? := null;
    var t0: Task<int>? := null;
    var cs0: CancellationTokenSource? := null;
    var ct0: CancellationToken? := null;
    var ie0: IEnumerable<int>? := null;
    var cstr0: CString? := null;
    var dt0: DateTime? := null;
    var f0: ((int) -> int)? := null;
    var sb0: StringBuilder? := null;
    Console.WriteLine($"{d0}|{c0}|{r0}|{a0}|{st0}|{m0}|{t0}");
    Console.WriteLine($"{cs0}|{ct0}|{ie0}|{cstr0}|{dt0}|{f0}|{sb0}");
  end Main;
end Program;
