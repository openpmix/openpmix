#!/usr/bin/env python3
#
# Copyright (c) 2022      Nanook Consulting. All rights reserved

from distutils.core import setup
from distutils.extension import Extension
from Cython.Build import cythonize
from sys import platform, maxsize, version_info
import os, sys
from Cython.Compiler.Main import default_options, CompilationOptions
default_options['emit_linenums'] = True
from subprocess import check_output, CalledProcessError

def get_include():
    dirs = []
    try:
        dirs.append(os.environ['PMIX_BINDINGS_TOP_BUILDDIR'] + "/include")
        dirs.append(os.environ['PMIX_BINDINGS_TOP_SRCDIR'] + "/include")
    except:
        return dirs
    return dirs
    
def getVersion():
    dir = os.path.dirname(__file__)

    vers_path = os.path.join(dir, '../../include', 'pmix_version.h')
    if not os.path.exists(vers_path):
        include_dirs = get_include()
        vers_path = None
        for dir in include_dirs:
            tmp_path = os.path.join(dir, 'pmix_version.h')
            if os.path.exists(tmp_path):
                vers_path = tmp_path
                break
        if vers_path is None:
            print("Error: pmix_version.h does not exist at path: ",vers_path)
            sys.exit(1)

    with open(vers_path) as verFile:
        lines = verFile.readlines()
        for l in lines:
            if 'MAJOR' in l:
                major = l.split()[2]
                major = major[:-1]
            elif 'MINOR' in l:
                minor = l.split()[2]
                minor = minor[:-1]
            elif 'RELEASE' in l:
                release = l.split()[2]
                release = release[:-1]
        vers = [major, minor, release]
        version = ".".join(vers)
        return version

def package_setup(package_name, package_vers):
    '''
    Package setup routine.
    '''
    setup(
        name = 'pypmix',
        version = getVersion(),
        url = 'https://pmix.org',
        license = '3-clause BSD',
        author = 'Ralph H. Castain',
        author_email = 'rhc@pmix.org',
        description = 'Python bindings for PMIx',
        classifiers = [
                'Development Status :: 5 - Production/Stable',
                'Intended Audience :: Developers',
                'Topic :: HPC :: Parallel Programming :: System Management',
                'License :: OSI Approved :: BSD License',
                'Programming Language :: Python :: 3.4',
                'Programming Language :: Python :: 3.5',
                'Programming Language :: Python :: 3.6',
                'Programming Language :: Python :: 3.7',
                'Programming Language :: Python :: 3.8',
                'Programming Language :: Python :: 3.9',
                'Programming Language :: Python :: 3.10',
                'Programming Language :: Python :: 3.11',
                'Programming Language :: Python :: 3.12'],
        keywords = 'PMI PMIx HPC MPI SHMEM',
        platforms = 'any',
        ext_modules = cythonize([Extension("pmix",
                                           [os.environ['PMIX_BINDINGS_TOP_SRCDIR']+"/bindings/python/pmix.pyx"],
                                           libraries=["pmix"]) ],
                                compiler_directives={'language_level': 3}),
        include_dirs = get_include()
    )

def main():
    '''
    The main entry point for this program.
    '''
    package_name = 'pypmix'
    package_vers = getVersion()

    package_setup(package_name, package_vers)

    return os.EX_OK


if __name__ == '__main__':
    sys.exit(main())
