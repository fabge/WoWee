#pragma once

namespace wowee::network {

/// Which cipher the world connection's packet headers use.
///
/// Its own header so that both ends can name it without depending on each
/// other: `game/game_handler.hpp` only forward-declares `WorldSocket`, and
/// `src/network/` must not know what an expansion is.
///
/// The choice belongs to the expansion profile - see
/// `ExpansionProfile::headerCrypt` - and travels down to the socket with the
/// session key. It used to be worked out inside `WorldSocket::initEncryption`
/// from hard-coded build numbers, which made the network layer the one place
/// that knew which expansions exist.
enum class HeaderCrypt {
    VanillaXor,   ///< 1.x: XOR-and-add over the raw session key.
    TbcHmacXor,   ///< 2.x: the same cipher, keyed by HMAC-SHA1 of the session key.
    WotlkRc4,     ///< 3.x: RC4-drop1024, separate send and receive keys.
};

}  // namespace wowee::network
