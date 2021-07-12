# NAME

pps - Request the status of one or more namespaces and/or nodes

# SYNOPSIS

```
pps [ options ]
```

# DESCRIPTION

`pps` will request the status of the specified namespaces and/or nodes. If no namespace or node is given, then `pps` will request the status of all running jobs. This requires that `pps` connect to an appropriate server - the tool will automatically attempt to do so, but options are provided to assist its search or to direct it to a specific server.


# OPTIONS

 `pps` supports the following options:

```
-h|--help                This help message
-n|--nodes               Display Node Information
   --nspace              Nspace of job whose status is being requested
-p|--pid <arg0>          Specify pid of starter to be contacted (default is
                         to system server
   --parseable           Provide parseable output
```

# AUTHORS

The OpenPMIx maintainers -- see
[https://github.com/openpmix/openpmix](https://github.com/openpmix/openpmix)
or the file `AUTHORS`.
