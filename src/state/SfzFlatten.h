// Flattens a real multi-region .sfz instrument (resolves #define/#include/
// default_path and the <global>/<master>/<group>/<region> cascade down to
// independent <region> blocks) for the Sample tab's "Multisample SFZ" mode -
// see proto/sfzflat.py, the Python reference this is a faithful C++ port of.
#pragma once

#include <string>

struct FlattenedSfz {
    bool ok = false;
    std::string error;       // set iff !ok: missing file, circular include,
                              // zero regions found, etc.
    std::string regionsText; // concatenated "<region> key=val ...\n" blocks,
                              // cleaned to the basic opcode allowlist, with
                              // every sample= already rewritten relative to
                              // SoloSampler's virtual "/" root - ready to
                              // splice into buildSfzText verbatim.
    int regionCount = 0;
};

// GUI thread only (file dialog accept / XDND drop) - same threading
// convention as analyzeSampleFile() in SampleInfo.h.
FlattenedSfz flattenMultisampleSfz(const std::string& sfzPath);
