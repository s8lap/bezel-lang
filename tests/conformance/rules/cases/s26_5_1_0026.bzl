//! case: s26_5_1_0026
//! doc: spec
//! section: 26.5.1
//! title: user ext IEnumerable<int> and library ext IEnumerable<T> are ambiguous at IEnumerable<int>
//! rule: If two equally specific extensions match, the call is a compile error; candidates with identical substituted heads never auto-rank, so extension IEnumerable<int> and the library's extension IEnumerable<T> are indistinguishable for an IEnumerable<int> receiver (spec 26.5 rule 4 and 26.5.1, v0.350).
//! cites: 26.5.1, 26.5
//! expect: reject
//! diagnostic: ambiguous
module R;
extension IEnumerable<int> public method Sum(): int => 77; end;
class Program public static method Main(): int
  var a: Array<int> := [1,2,3]; var e: IEnumerable<int> := a;
  Console.WriteLine(e.Sum());
  return 0;
end Main; end Program;
