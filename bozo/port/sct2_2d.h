#pragma once

namespace bozo::port
{
    class Registry;
}

namespace sct2::draw2d
{
    void install(bozo::port::Registry &runtime);
    // The 2D the game made this frame, drawn in its ordering table's order and published as the frame's overlay.
    void publish();
}
