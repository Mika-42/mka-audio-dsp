#include <iostream>
#include <vector>
#include <cstdlib>

import mka.audio.dsp.graph;

int main()
{
    mka::audio::dsp::DAGraph graph;

    const mka::audio::dsp::NodeID test1 = graph.addNode("test");
    const mka::audio::dsp::NodeID test2 = graph.addNode("test2");
    const mka::audio::dsp::NodeID test3 = graph.addNode("test3");

    graph.connect(test1, test2);
    graph.connect(test2, test3);
    
    return 0;
}