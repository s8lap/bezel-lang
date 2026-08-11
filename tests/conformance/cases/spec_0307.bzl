var resp := new Response(headers, stream);
return FetchOutcome.Fetched(move resp);       // call-site marker; resp is moved-from after
