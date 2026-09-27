"""Keep native pixel faces byte-for-byte aligned with the web's authored 5x7 grid."""
import json
from pathlib import Path
root = Path(__file__).resolve().parents[2]
data = json.loads((root / "web/src/lib/sdui/pixel-digits.json").read_text())
out = "// Generated from web/src/lib/sdui/pixel-digits.json\n#pragma once\n#include <stdint.h>\n"
out += "static constexpr uint8_t pixel_digits[11][7] = {\n"
out += ",\n".join("{" + ",".join(map(str, data[c])) + "}" for c in "0123456789-")
(root / "firmware/include/fonts/pixel_digits.h").write_text(out + "\n};\n")
