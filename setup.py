# based on https://github.com/pybind/cmake_example/blob/master/setup.py
import os
import re
import subprocess
import sys
import shutil
from pathlib import Path

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

binary_names = ["circt-opt", "firtool"]


# A CMakeExtension needs a sourcedir instead of a file list.
# The name must be the _single_ output extension from the CMake build.
# If you need multiple extensions, see scikit-build.
class CMakeExtension(Extension):
    def __init__(self, name: str, sourcedir: str = "") -> None:
        super().__init__(name, sources=[])
        self.sourcedir = os.fspath(Path(sourcedir).resolve())


class CMakeBuild(build_ext):
    def build_extension(self, ext: CMakeExtension) -> None:
        extdir = os.path.abspath(os.path.dirname(self.get_ext_fullpath(ext.name)))
        if not os.path.exists(extdir):
            os.makedirs(extdir)

        # we assume everything is already built
        bins = ["hgdb-rtl"]
        root_dir = os.path.dirname(__file__)
        binaries = [os.path.join(root_dir, "build", "bin", name) for name in binary_names]
        for binary in binaries:
            assert os.path.isfile(binary)
            shutil.copy(binary, extdir)


with open(os.path.join(os.path.dirname(__file__), "README.md")) as f:
    long_description = f.read()

# The information here can also be placed in setup.cfg - better separation of
# logic and declaration, and simpler if you include description/version in a file.
setup(
    name="hgdb-circt",
    version="0.0.1",
    author="Keyi Zhang",
    author_email="keyi@cs.stanford.edu",
    url="https://github.com/Kuree/hgdb-circt",
    description="Circt binary patched with hgdb",
    long_description=long_description,
    long_description_content_type="text/markdown",
    ext_modules=[CMakeExtension("hgdb-circt")],
    cmdclass={"build_ext": CMakeBuild},
    zip_safe=False,
    scripts=[os.path.join("scripts", name) for name in binary_names],
    extras_require={"test": ["pytest>=6.0"]},
    python_requires=">=3.6",
)
