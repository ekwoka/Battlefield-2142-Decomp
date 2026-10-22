# Writes the Python sources the host runs into a header as raw strings.
# -DOUT=<header> -DPY2COMPAT=<py2compat.py> -DHOSTOBJECTS=<hostobjects.py>
file(READ "${PY2COMPAT}" compat)
file(READ "${HOSTOBJECTS}" objects)
file(WRITE "${OUT}" "// Generated from src/Python/*.py by embed.cmake.\n#pragma once\n")
file(APPEND "${OUT}" "static const char kPy2Compat[] = R\"BF2142PY(${compat})BF2142PY\";\n")
file(APPEND "${OUT}" "static const char kHostObjects[] = R\"BF2142PY(${objects})BF2142PY\";\n")
