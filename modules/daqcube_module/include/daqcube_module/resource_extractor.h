#pragma once

#include <string>

namespace daq::modules::daqcube_module
{
    // Extracts one embedded RCDATA resource (see embedded_resources.h) to
    // %LOCALAPPDATA%\DAQCube\<module version>\<fileName>, skipping the
    // write when the file is already there with the right size, and returns
    // its path. Extraction is per resource so a session only pays for the
    // core it actually runs.
    bool extractEmbeddedResource(int resourceId, const char* fileName, std::string& pathOut, std::string& errorOut);
}
