module;
#include <vector>

export module mka.audio.dsp.graph_snapshot;
export import mka.audio.dsp.connection;

export namespace mka::audio::dsp::detail {
    /**
     * @brief Independent control-side values describing desired graph structure.
     * @details Owns its identity/edge/marker vectors, never Nodes or samples.
     * Editing a detached snapshot cannot mutate a DAG or prepared plan. No
     * callback consumes these vectors. The topology helper deliberately ignores
     * outputs; output-role and Config validation belongs to compilation.
     */
    struct GraphSnapshot {
        std::vector<NodeID> nodes; ///< Desired Node identities; order is not semantic.
        std::vector<Connection> connections; ///< Complete logical edge values.
        std::vector<NodeID> outputs; ///< Desired output identities, not physical lanes.
    };
}
