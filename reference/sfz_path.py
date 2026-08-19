def insert_path(default_path, _sfz):

    sfz = _sfz.replace(chr(92), "/") # stupid backslashes!!!

    # value goes until the next opcode= or end of line, since sample paths can contain spaces
    patterns = [
        r"sample=(.*?)(?=\s+\w+=|\r?\n|$)",
    ]

    for p in patterns:
        samples = re.findall(p, sfz)
        if len(samples) != 0:
            break

    #default_paths = re.findall(r"default_path=([^\n]*)", sfz, flags=re.MULTILINE)

    #includes = re.findall(r'#include\s+"([^"]+)"', sfz)

    #inls = []
    #dels = []
    smls = []
    #for i in includes:
    #    inls.append((i, f"{default_path}{i}"))
    #for i in default_paths:
    #    dels.append((i, f"{default_path}{i}"))

    for i in samples:
        smls.append((i, f"{default_path}{i}"))

    r1 = replace_substrings_safe(sfz, smls)
    #r2 = replace_substrings_safe(r1, dels)

    #print(r2)
    return r1

def get_relative_path(file_path, _preset_path):
  # path of the sample/include relative to the folder the preset (sfz) is saved in
  # SFZ files always use "/" as separator regardless of OS, so normalize to that
  preset_dir = os.path.dirname(_preset_path)
  rel_path = os.path.relpath(file_path, preset_dir)
  return rel_path.replace(os.sep, "/")
