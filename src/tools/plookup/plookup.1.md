# NAME

plookup - Request the value of one or more keys

# SYNOPSIS

```
plookup [ options ] [ keys ]
```

# DESCRIPTION

`plookup` will request the value of the specified keys. This requires that `plookup` connect to an appropriate server - the tool will automatically attempt to do so, but options are provided to assist its search or to direct it to a specific server. Reserved keys must be provided in their string form - e.g., "pmix.job.term.status" as opposed to the key's name of "PMIX_JOB_TERM_STATUS".


# OPTIONS

 `plookup` supports the following options:

```
-h|--help                This help message
-p|--pid <arg0>          Specify starter pid
-t|--timeout             Max number of seconds to wait for data to become
                         available
-v|--verbose             Be Verbose
-w|--wait                Wait for data to be available
```

# AUTHORS

The OpenPMIx maintainers -- see
[https://github.com/openpmix/openpmix](https://github.com/openpmix/openpmix)
or the file `AUTHORS`.
