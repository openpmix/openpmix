#!/opt/local/bin/python

from pmix import *

def main():
    try:
        foo = PMIxClient()
    except:
        print("FAILED TO CREATE CLIENT")
        exit(1)
    print("MYVERS ", foo.get_version())
    print("INITD ", foo.initialized())
    dict = {'FOOBAR': 'VAR', 'BLAST': 7}
    my_result = foo.init(dict)
    print("MYRES ", my_result)

if __name__ == '__main__':
    main()
