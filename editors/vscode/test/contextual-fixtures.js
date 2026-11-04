// Shared fixtures for the contextual-keyword tests (test/contextual.js for the TextMate grammar,
// test/hljs.js for the docs site's highlight.js language). Each case is a Bezel snippet in which
// «word» marks a contextual keyword that must be coloured as a keyword in that position, and
// ‹word› marks the same spelling used as an identifier, which must stay plain. The positions are
// spec Appendix B.2's, with the Appendix A production each comes from.
module.exports = [
  // of: the scrutinee/arm delimiter of a `case` (A.6 CaseStmt, A.7 CaseExpr)
  'case x «of»',
  'var y := (case f(z) «of» 1 => 2, _ => 3);',
  'case Classify(\n    shape) «of»\n  _ => 0\nend case;',
  'case n\n«of»\n  1 => A();\nend case;',
  'var ‹of› := 1;',
  'if ‹of› is Foo then return 4; end;',
  'case n\n  1 => Console.WriteLine(‹of›);,\nend case;',
  'Print(‹of›); // case x of',
  // step: after a range (A.7 Range)
  'for var i in 1..10 «step» 2 do',
  'var fives := 0..<50 «step» 5;',
  'var r := 0..Count(xs) «step» k;',
  'var ‹step› := 3;',
  'for var i in 0..n do Foo(‹step› + 1); end;',
  'Run(‹step›);',
  'var r := 10.. ‹step› 1;',
  'var r := 0..‹step›;',
  // convention: after the library name of an extern block header (A.3 ExternBlock)
  'extern "user32" «convention» "stdcall"\n  public method MessageBoxA(h: IntPtr): int;\nend;',
  'var ‹convention› := "cdecl";',
  'extern "libc"\n  public method f(‹convention›: int): int;\nend;',
  // symbol, variadic: the extern alias clause (A.3 ExternDecl, VariadicClause)
  'extern "libc"\n  public method open2(p: CString, f: int): int\n      := «symbol» "open" «variadic»(2);\nend;',
  'extern "libexample"\n  method GetVersion(): int := «symbol» "example_get_version";\nend;',
  'var ‹symbol› := Lookup("x");',
  'Call(‹variadic›(2));',
  // env: the user-data marker in an extern parameter list and in its callback type (A.3 ExternParam)
  'extern "libc"\n  public method qsort_r(b: Ptr<byte>, n: UIntPtr,\n      compar: (IntPtr, IntPtr, «env») -> int, «env»): ();\nend;',
  'extern "sqlite3"\n  public method exec(db: IntPtr,\n      cb: («env», int) -> int,\n      «env»,\n      errmsg: Ptr<IntPtr>): int;\nend;',
  'extern "libc"\n  public method g(‹env›: IntPtr): int;\nend;',
  'Native.Call(cb, ‹env›);',
  // serialized (+ skip, optional): the member clause (A.3 SerializedClause)
  'record Doc(Title: string, Rank: Level «serialized» «skip»);',
  'record Money(Amount: decimal, Cur: string «serialized» "currency" «optional» «equality» «skip»);',
  'public property Name: string «serialized» "name";',
  'var ‹serialized› := Encode(x); var ‹skip› := 1; var ‹optional› := true;',
  // equality skip (A.3/A.4 EqualityClause)
  '  private «equality» «skip» _count: int;',
  'var ‹equality› := a = b;',
  // polymorphic: the class-body registry member (A.3 PolymorphicMember)
  '  «polymorphic» ("circle": Circle, "square": Square);',
  '‹polymorphic›(shape);',
  // operator: after `static method`, in an interface's `static operator`, in `end operator` (A.4)
  'public static method «operator» +(a: Money, b: Money): Money => a;',
  'public static pure method «operator» <>(a: P, b: P): bool => false;',
  '  static «operator» *(a: T, b: T): owned T;',
  'public static method «operator» -(a: V): V\n  return a;\nend «operator» -;',
  'var ‹operator› := "+";',
  'public method ‹operator›(x: int): int => x;',
  // scoped: field/parameter modifier and call-site marker (A.4 FieldDecl, Param; §28.14)
  'public constructor(«scoped» cb: () -> ())',
  '  private «scoped» _cb: () -> ();',
  'using var r := new Reader(«scoped» _s);',
  'var ‹scoped› := 1; Use(‹scoped›);',
  // pure: the member-modifier slot and `pure get` (A.4)
  'public «pure» method Area(): double => 1.0;',
  '  «pure» «get» => _x;',
  'public static «pure» method Max(a: int, b: int): int => a;',
  'var ‹pure› := Check(x);',
  // get, set: accessor heads and the auto-property shorthand (A.4 Accessor, AutoAccessorVis)
  '  «get» => _name;',
  '  «private» «set» => _name := value;',
  '  «set»(owned) => do _items[i] := value; end;',
  'public property Id: int private «set»;',
  'var ‹get› := 1; ‹set› := ‹get› + 1;',
  'return ‹get›;',
  // old: inside an `ensures (...)` clause (A.4 ContractClause)
  '  ensures (Count = «old»(Count) + 1)',
  '  ensures (Max(«old»(a), b) > 0)',
  'var x := ‹old›(1);',
  // value, notnull, sendable, hashable: a `where T:` constraint (A.5 Constraint)
  'public class Box<T> where T: «value»',
  'method Spawn<A, R>(f: (A) -> R) where A: «sendable», «notnull» where R: «sendable», class',
  'record Key<K> where K: «hashable»;',
  'var ‹value› := 1; var ‹notnull› := true; var ‹sendable› := 0; var ‹hashable› := 0;',
  'public property X: int\n  set => _x := ‹value›;\nend X;',
  // result: deliberately left plain (its reading depends on the enclosing `ensures`)
  '  ensures (‹result› > 0)',
];
