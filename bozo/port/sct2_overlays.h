#pragma once

struct Runtime;

namespace sct2::overlays
{
    // The code overlays the game unpacks over .staytrash, each registered as a module told apart by the code it holds
    // (tools/sct2_overlays.py writes it from the disc).
    void attach(Runtime &rt);
}
