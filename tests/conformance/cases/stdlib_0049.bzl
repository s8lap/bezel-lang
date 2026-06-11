if not pool.Contains(candidate) then
  pool.Add(candidate);     // ownership transfers
end;
// On the duplicate path, candidate retains ownership.
