import FFIExample;

using var copy := FFIExample.strdup(cname);   // owned CString? — discharge it (§32.6)
