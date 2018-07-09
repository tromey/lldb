import distutils.sysconfig
import os
import platform
import re
import sys


def get_python_libdir_suffix():
    """Returns the appropropriate python libdir suffix.

    @return the python libdir suffix, normally either "" or "64".
    """
    if platform.system() != 'Linux':
        return ""

    # We currently have a bug in lldb -P that does not account for
    # architecture variants in python paths for
    # architecture-specific modules.  Handle the lookup here.
    # When that bug is fixed, we should just ask lldb for the
    # right answer always.
    arch_specific_libdir = distutils.sysconfig.get_python_lib(True, False)
    split_libdir = arch_specific_libdir.split(os.sep)
    lib_re = re.compile(r"^lib.+$")

    for i in range(len(split_libdir)):
        match = lib_re.match(split_libdir[i])
        if match is not None:
            return split_libdir[i][3:]
    return ""

if __name__ == '__main__':
    sys.stdout.write(get_python_libdir_suffix())
    sys.exit(0)
