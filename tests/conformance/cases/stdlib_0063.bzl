if not pool.Contains(candidate) then
  pool.Add(move candidate);    // ownership transfers (call-site marker)
end;
// On the duplicate path, candidate retains ownership.
