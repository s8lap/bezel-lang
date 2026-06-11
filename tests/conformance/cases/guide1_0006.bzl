method CloneBuffer(): owned Buffer
  var fresh := new Buffer();   // owned, because `new` produces an owned value
  fresh.CopyFrom(_source);
  return fresh;                // the owned value reaches the `owned` return — done
end CloneBuffer;
