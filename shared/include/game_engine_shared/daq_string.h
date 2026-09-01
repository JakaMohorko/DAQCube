#pragma once

#include <coretypes/stringobject.h>
#include <coretypes/objectptr.h>

#include <string>

namespace game_engine
{
    // null-safe IString -> std::string (unassigned or broken refs become "")
    inline std::string toStd(const daq::ObjectPtr<daq::IString>& str)
    {
        daq::ConstCharPtr chars = nullptr;
        if (!str.assigned() || OPENDAQ_FAILED(str->getCharPtr(&chars)) || !chars)
            return {};
        return chars;
    }
}
