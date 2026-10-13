# a file as a byte array: kOverlayFont / kOverlayFontSize
file(READ ${IN} hex HEX)
string(LENGTH "${hex}" n)
math(EXPR size "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUT} "#include <cstddef>\nextern const unsigned char kOverlayFont[] = {${bytes}};\nextern const std::size_t kOverlayFontSize = ${size};\n")
