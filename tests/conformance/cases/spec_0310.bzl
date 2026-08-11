case client.Fetch(url) of
  Fetched(_)  => Count("hit"),     // owned payload never bound
  _           => Count("other")
end;
