public static method Load(paths: IEnumerable<string>): IEnumerable<Doc>
  for var p in paths do
    using var d := Parse(p);      // Parse(p: string): owned Doc — frame-owned
    yield return d;               // adopt-then-yield: a borrow of a frame-owned binding
  end;
end Load;

var first := Load(paths).First();  // WARNS — spec §29.11's adopting arm
first.Title;                       // checked mode: DisposedAccessException — §29.7's
                                   //   pass disposed d when First() disposed the enumerator
