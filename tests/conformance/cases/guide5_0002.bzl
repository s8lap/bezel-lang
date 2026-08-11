if not seen.Contains(candidate) then
  seen.Add(move candidate);      // ownership transfers (call-site marker, Guide 1 §1)
end;
// on the duplicate path, candidate is still yours
