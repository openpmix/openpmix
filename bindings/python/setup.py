from distutils.core import setup
from distutils.extension import Extension
from Cython.Build import cythonize
from sys import platform, maxsize, version_info
import os
from subprocess import check_output, CalledProcessError

def getVersion():
    dir = os.path.dirname(__file__)
    vers_path = os.path.join(dir, '../../include', 'pmix_version.h')
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

def getLongDescription():
    dir = os.path.dirname(__file__)
    readmepath = os.path.join(dir, '../../README.md')
    pyver = version_info.major
    try:
        readme = check_output('pandoc {} -f markdown -t rst'.format(readmepath).split())
        if pyver == 3:
            readme = readme.decode()
        return readme
    except CalledProcessError:
        return "Not Available"

setup(
    name = 'pypmix',
    version = getVersion(),
    url = 'https://pmix.org',
    license = '3-clause BSD',
    author = 'Ralph H. Castain',
    author_email = 'ralph.h.castain@intel.com',
    description = 'Python bindings for PMIx',
    long_description = getLongDescription(),
    classifiers = [
            'Development Status :: 1 - Under Construction',
            'Intended Audience :: Developers',
            'Topic :: HPC :: Parallel Programming :: System Management',
            'License :: 3-clause BSD',
            'Programming Language :: Python :: 3.4',
            'Programming Language :: Python :: 3.5',
            'Programming Language :: Python :: 3.6'],
    keywords = 'PMI PMIx HPC MPI SHMEM',
    platforms = 'any',
    ext_modules = cythonize([Extension("pmix", ["pmix.pyx"], libraries=["pmix"])],
                            compiler_directives={'language_level': 3})
)
