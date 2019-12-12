# test cython code
#file: cython_test_functions.pyx

# TODO: add lots more types to load/test

pyinfo = [{'key': PMIX_EVENT_HDLR_NAME, 'value': 'SIMPCLIENT-MODEL',
           'val_type': PMIX_STRING}]
darray = {'type':PMIX_INFO, 'array':[{'key':PMIX_ALLOC_NETWORK_ID, 'value':'SIMPSCHED.net', 'val_type':PMIX_STRING},
                                     {'key':PMIX_ALLOC_NETWORK_SEC_KEY, 'value':'T', 'val_type':PMIX_BOOL},
                                     {'key':PMIX_SETUP_APP_ENVARS, 'value':'T', 'val_type':PMIX_BOOL}]
         }

values = [{'value': 'True', 'val_type': PMIX_BOOL},
          {'value': 1, 'val_type': PMIX_BYTE},
          {'value': "foo", 'val_type': PMIX_STRING},
          {'value': 45, 'val_type': PMIX_SIZE},
          {'value': 3, 'val_type': PMIX_PID},
          {'value': 11, 'val_type': PMIX_INT},
          {'value': 2, 'val_type': PMIX_INT8},
          {'value': 127, 'val_type': PMIX_INT16},
          {'value': 100, 'val_type': PMIX_INT32},
          {'value': 500, 'val_type': PMIX_INT64},
          {'value': 250, 'val_type': PMIX_UINT},
          {'value': 201, 'val_type': PMIX_UINT8},
          {'value': 700, 'val_type': PMIX_UINT16},
          {'value': 301, 'val_type': PMIX_UINT32},
          {'value': 301, 'val_type': PMIX_UINT64},
          {'value': 301.1, 'val_type': PMIX_FLOAT},
          {'value': 201.2, 'val_type': PMIX_DOUBLE},
          {'value': {'sec': 2, 'usec': 3}, 'val_type': PMIX_TIMEVAL},
          {'value': 100, 'val_type': PMIX_TIME},
          {'value': 34, 'val_type': PMIX_STATUS},
          {'value': 15, 'val_type': PMIX_PROC_RANK},
          {'value': {'nspace': 'testnspace', 'rank': 0}, 'val_type': PMIX_PROC},
          {'value': {'bytes': bytearray(1), 'size': 1}, 'val_type': PMIX_BYTE_OBJECT},
          {'value': 18, 'val_type': PMIX_PERSISTENCE},
          {'value': 1, 'val_type': PMIX_SCOPE},
          {'value': 5, 'val_type': PMIX_RANGE},
          {'value': 45, 'val_type': PMIX_PROC_STATE},
          {'value': {'proc': {'nspace': 'fakenspace', 'rank': 0}, 'hostname': 'myhostname', 'executable': 'testexec', 'pid': 2, 'exitcode': 0, 'state': 0}, 'val_type': PMIX_PROC_INFO},
          {'value': darray, 'val_type': PMIX_DATA_ARRAY},
          {'value': 19, 'val_type': PMIX_ALLOC_DIRECTIVE},
          {'value': {'envar': 'TEST_ENVAR', 'value': 'TEST_VAL', 'separator': ':'},
           'val_type': PMIX_ENVAR}
          # TODO: is regex type a string? It is not in the type conversion 
          # table of pg 357 of the v4 API documentationi for python
        ]

def error_string(pystat:int):
    cdef char *string
    string = <char*>PMIx_Error_string(pystat)
    pystr = string
    val = pystr.decode('ascii')
    return val

def test_load_value():
    global values
    cdef pmix_value_t value
    for val in values:
        rc = pmix_load_value(&value, val)
        if rc == PMIX_SUCCESS:
            print("SUCCESSFULLY LOADED VALUE:\n")
            print("\t" + str(val['value']) + ", "
                  + str(PMIx_Data_type_string(val['val_type'])) + "\n") 

def test_alloc_info():
    global pyinfo, values
    cdef pmix_info_t *info
    cdef pmix_info_t **info_ptr
    cdef size_t ninfo

    # call pmix_alloc_info
    info_ptr = &info
    rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
    if rc == PMIX_SUCCESS:
        print("SUCCESSFULLY LOADED:\n")
        for i in pyinfo:
            print("\t" + str(i['key']) + ", " + str(i['value']) + ", " +
                  str(PMIx_Data_type_string(i['val_type'])) + "\n")
