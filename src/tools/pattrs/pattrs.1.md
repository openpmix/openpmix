# NAME

pattrs - Print supported attributes at the client, server, and host levels

# SYNOPSIS

```
pattrs [ options ]
```

# DESCRIPTION

`pattrs` will print a list of the supported PMIx APIs and the supported PMIx attributes for each PMIx API at each of the client, server, and host levels. Host level information requires that `pattrs` connect to an appropriate server - the tool will automatically attempt to do so, but options are provided to assist its search or to direct it to a specific server.


# OPTIONS

 `pattrs` supports the following options:

```
-c|--client <arg0>       Comma-delimited list of client function whose
                         attributes are to be printed (function or all)
   --client-fns          List the functions supported in this client
                         library
-h|--help                This help message
-h|--host <arg0>         Comma-delimited list of host function whose
                         attributes are to be printed (function or all)
   --host-fns            List the functions supported by this host
                         environment
-n|--nspace <arg0>       Specify server nspace to connect to
-p|--pid <arg0>          Specify server pid to connect to
-s|--server <arg0>       Comma-delimited list of server function whose
                         attributes are to be printed (function or all)
   --server-fns          List the functions supported in this server
                         library
   --system-server       Specifically connect to the system server
   --system-server-first Look for the system server first
-t|--tool <arg0>         Comma-delimited list of tool function whose
                         attributes are to be printed (function or all)
   --tool-fns            List the functions supported in this tool library
   --uri <arg0>          Specify URI of server to connect to
-v|--verbose             Be Verbose

```

# AUTHORS

The OpenPMIx maintainers -- see
[https://github.com/openpmix/openpmix](https://github.com/openpmix/openpmix)
or the file `AUTHORS`.
