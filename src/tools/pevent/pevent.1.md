# NAME

pevent - Generate an event and inject it into the system

# SYNOPSIS

```
pevent [ options ]
```

# DESCRIPTION

`pevent` will generate a specified event and inject it into a server. This requires that `pevent` connect to an appropriate server - the tool will automatically attempt to do so, but options are provided to assist its search or to direct it to a specific server.


# OPTIONS

 `pevent` supports the following options:

```
-e|--event               Status code of event to be sent
-h|--help                This help message
-p|--pid <arg0>          Specify starter pid
-r|--range               Range of event to be sent
-v|--verbose             Be Verbose
```

# AUTHORS

The OpenPMIx maintainers -- see
[https://github.com/openpmix/openpmix](https://github.com/openpmix/openpmix)
or the file `AUTHORS`.
