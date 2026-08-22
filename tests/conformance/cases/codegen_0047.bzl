method Make(): owned Buffer
  using var fresh := new Buffer();
  return fresh;                    // ERROR: cannot move out of a `using var`
end Make;
