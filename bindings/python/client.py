#!/opt/local/bin/python

from pmix import *

def main():
    foo = PMIxClient()
    print("Testing PMIx ", foo.get_version())
    info = {PMIX_PROGRAMMING_MODEL: ('TEST', 'string'), PMIX_MODEL_LIBRARY_NAME: ("PMIX", 'string')}
    my_result = foo.init(info)
    print("Init result ", my_result)
    if 0 != my_result:
        print("FAILED TO INIT")
        exit(1)
    # try getting something

    # finalize
    info = {}
    foo.finalize(info)
if __name__ == '__main__':
    main()
