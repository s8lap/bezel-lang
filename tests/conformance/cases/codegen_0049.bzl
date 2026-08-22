var x: Buffer;
if useFresh then
  x := new Buffer();               // OWNED here
else
  x := registry.Buffer;            // BORROWED here
end;                               // ERROR at the merge
var s := dest ?? new FileStream(path, FileMode.Create);   // ERROR: same rejection as an expression
