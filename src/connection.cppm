export module mka.audio.dsp.connection;
export import mka.audio.dsp.channel;

export namespace mka::audio::dsp {
    /**
     * @brief Value identity of one logical edge: producer Out -> consumer In/Aux.
     * @details DAG owns copies of these values, independently of Node storage.
     * Equality compares both complete ChannelIDs, including their port/index
     * fields. Copies may be edited independently; no Node or audio buffer is
     * owned, borrowed or kept alive by a Connection. No hardware lane is encoded.
     * A standalone aggregate (including its zero default) is not validated;
     * graph insertion validates direction, existence, occupancy and acyclicity
     * on control. No allocation, sample access or RT ownership operation.
     * Field roles are independent of the argument convention of connect().
     */
    struct Connection {
        ChannelID from{}; ///< Producer's complete logical Out identity, stored by value.
        ChannelID to{};   ///< Consumer's complete logical In/Aux identity, stored by value.

        constexpr bool operator==(const Connection&) const noexcept = default;
    };
}
