public method ReplaceAt(index: int, owned newValue: T)
  var old := move pool[index];      // take from slot
  pool[index] := newValue;           // restore slot with new value
  old.Dispose();                     // dispose the old value
  // (or `using var taken := move pool[index];` for scope-bound disposal)
end ReplaceAt;
