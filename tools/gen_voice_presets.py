# Emit the C++ preset table used by the GUI's "generate built-in voices" button.
# Source of truth: the legacy Python server's voice_presets.py (44 entries).
#
# Usage:
#   python tools\gen_voice_presets.py --src <path to the Python server's server
#                                          directory containing voice_presets.py>
#                                    [--out src\voice_presets.inc]
#
# The generated file only carries voice ids and their Chinese text descriptions;
# no audio, no machine-specific paths.
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

ap = argparse.ArgumentParser()
ap.add_argument("--src", required=True, help="directory containing voice_presets.py")
ap.add_argument("--out", default=os.path.join(ROOT, "src", "voice_presets.inc"))
args = ap.parse_args()

sys.path.insert(0, os.path.abspath(args.src))
from voice_presets import PRESETS, DEFAULT_LANGUAGE  # noqa: E402


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


lines = []
lines.append("// =============================================================================")
lines.append("//  voice_presets.inc - built-in voice designs (GENERATED, do not edit by hand)")
lines.append("//")
lines.append("//  Copied from the legacy Python server so both implementations offer the same")
lines.append("//  44 presets (voice id + Chinese description; no audio is embedded).")
lines.append("//  Regenerate with: python tools\\gen_voice_presets.py --src <dir with voice_presets.py>")
lines.append("// =============================================================================")
lines.append("struct VoicePreset {")
lines.append("    const char* id;")
lines.append("    const char* description;")
lines.append("};")
lines.append("")
lines.append("static const VoicePreset kVoicePresets[] = {")
for p in PRESETS:
    lines.append('    {%s, %s},' % (cstr(p["id"]), cstr(p["description"])))
lines.append("};")
lines.append("static const char* const kVoicePresetLanguage = %s;" % cstr(DEFAULT_LANGUAGE))
lines.append("")
with open(args.out, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines))
print("wrote %s  (%d presets, language=%s)" % (args.out, len(PRESETS), DEFAULT_LANGUAGE))
