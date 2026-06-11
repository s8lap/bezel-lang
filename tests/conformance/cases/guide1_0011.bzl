var x: Buffer;
if useFresh then
  x := new Buffer();              // x is OWNED here
else
  x := registry.Buffer;           // x is BORROWED here
end;
// ERROR at the merge: x is owned on one branch, borrowed on the other
