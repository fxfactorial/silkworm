/*
   Copyright 2020 The Silkworm Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "decode.hpp"

#include <cassert>
#include <silkworm/common/util.hpp>
#include <tuple>

namespace silkworm::rlp {

std::pair<uint64_t, DecodingError> read_uint64(ByteView be, bool allow_leading_zeros) noexcept {
    static constexpr size_t kMaxBytes{8};
    static_assert(sizeof(uint64_t) == kMaxBytes);

    uint64_t buf{0};

    if (be.length() > kMaxBytes) {
        return {buf, DecodingError::kOverflow};
    }

    if (be.empty()) {
        return {buf, DecodingError::kOk};
    }

    if (be[0] == 0 && !allow_leading_zeros) {
        return {buf, DecodingError::kLeadingZero};
    }

    auto* p{reinterpret_cast<uint8_t*>(&buf)};
    std::memcpy(p + (kMaxBytes - be.length()), &be[0], be.length());

    // We assume a little-endian architecture like amd64
    // TODO[C++20] static_assert(std::endian::order::native == std::endian::order::little);
    buf = intx::bswap(buf);
    return {buf, DecodingError::kOk};
}

std::pair<intx::uint256, DecodingError> read_uint256(ByteView be, bool allow_leading_zeros) noexcept {
    static constexpr size_t kMaxBytes{32};
    static_assert(sizeof(intx::uint256) == kMaxBytes);

    intx::uint256 buf{0};

    if (be.length() > kMaxBytes) {
        return {buf, DecodingError::kOverflow};
    }

    if (be.empty()) {
        return {buf, DecodingError::kOk};
    }

    if (be[0] == 0 && !allow_leading_zeros) {
        return {buf, DecodingError::kLeadingZero};
    }

    uint8_t* p{as_bytes(buf)};
    std::memcpy(p + (kMaxBytes - be.length()), &be[0], be.length());

    // TODO[C++20] static_assert(std::endian::order::native == std::endian::order::little);
    buf = intx::bswap(buf);
    return {buf, DecodingError::kOk};
}

std::pair<Header, DecodingError> decode_header(ByteView& from) noexcept {
    Header h;
    if (from.empty()) {
        return {h, DecodingError::kInputTooShort};
    }

    uint8_t b{from[0]};
    if (b < 0x80) {
        h.payload_length = 1;
    } else if (b < 0xB8) {
        from.remove_prefix(1);
        h.payload_length = b - 0x80;
        if (h.payload_length == 1) {
            if (from.empty()) {
                return {h, DecodingError::kInputTooShort};
            }
            if (from[0] < 0x80) {
                return {h, DecodingError::kNonCanonicalSingleByte};
            }
        }
    } else if (b < 0xC0) {
        from.remove_prefix(1);
        size_t len_of_len{b - 0xB7u};
        if (from.length() < len_of_len) {
            return {h, DecodingError::kInputTooShort};
        }
        auto [len, err]{read_uint64(from.substr(0, len_of_len))};
        if (err != DecodingError::kOk) {
            return {h, err};
        }
        h.payload_length = len;
        from.remove_prefix(len_of_len);
        if (h.payload_length < 56) {
            return {h, DecodingError::kNonCanonicalSize};
        }
    } else if (b < 0xF8) {
        from.remove_prefix(1);
        h.list = true;
        h.payload_length = b - 0xC0;
    } else {
        from.remove_prefix(1);
        h.list = true;
        size_t len_of_len{b - 0xF7u};
        if (from.length() < len_of_len) {
            return {h, DecodingError::kInputTooShort};
        }
        auto [len, err]{read_uint64(from.substr(0, len_of_len))};
        if (err != DecodingError::kOk) {
            return {h, err};
        }
        h.payload_length = len;
        from.remove_prefix(len_of_len);
        if (h.payload_length < 56) {
            return {h, DecodingError::kNonCanonicalSize};
        }
    }

    if (from.length() < h.payload_length) {
        return {h, DecodingError::kInputTooShort};
    }

    return {h, DecodingError::kOk};
}

template <>
[[nodiscard]] DecodingError decode(ByteView& from, Bytes& to) noexcept {
    auto [h, err]{decode_header(from)};
    if (err != DecodingError::kOk) {
        return err;
    }
    if (h.list) {
        return DecodingError::kUnexpectedList;
    }
    to = from.substr(0, h.payload_length);
    from.remove_prefix(h.payload_length);
    return DecodingError::kOk;
}

template <>
[[nodiscard]] DecodingError decode(ByteView& from, uint64_t& to) noexcept {
    auto [h, err1]{decode_header(from)};
    if (err1 != DecodingError::kOk) {
        return err1;
    }
    if (h.list) {
        return DecodingError::kUnexpectedList;
    }
    DecodingError err2;
    std::tie(to, err2) = read_uint64(from.substr(0, h.payload_length));
    if (err2 != DecodingError::kOk) {
        return err2;
    }
    from.remove_prefix(h.payload_length);
    return DecodingError::kOk;
}

template <>
[[nodiscard]] DecodingError decode(ByteView& from, intx::uint256& to) noexcept {
    auto [h, err1]{decode_header(from)};
    if (err1 != DecodingError::kOk) {
        return err1;
    }
    if (h.list) {
        return DecodingError::kUnexpectedList;
    }
    DecodingError err2;
    std::tie(to, err2) = read_uint256(from.substr(0, h.payload_length));
    if (err2 != DecodingError::kOk) {
        return err2;
    }
    from.remove_prefix(h.payload_length);
    return DecodingError::kOk;
}

}  // namespace silkworm::rlp
