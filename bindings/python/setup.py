from distutils.core import setup
from distutils.extension import Extension
from Cython.Build import cythonize

setup(
    name = 'pypmix',
    version = '3.0.0',
    url = 'https://pmix.org',
    license = '3-clause BSD',
    author = 'Ralph H. Castain',
    author_email = 'ralph.h.castain@intel.com',
    description = 'Python bindings for PMIx',
    platforms = 'any',
    ext_modules = cythonize([Extension("pmix", ["pmix.pyx"],
                                       libraries=["pmix"])
    ])
)
