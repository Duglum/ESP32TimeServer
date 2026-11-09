// ESP32 Time Server (from ESP32-ConsoleServer)
// Copyright Duglum, 2026. License: MIT (see LICENSE)
//

#include "auth/radius_proto.h"

#include <cstdio>
#include <cstring>
#include <strings.h>

extern "C"
{
#include "psa/crypto.h"
}

namespace
{
    constexpr size_t MD5_SIZE = 16;
    constexpr size_t MA_ATTRIBUTE = 2 + MD5_SIZE;

    bool md5_parts(const uint8_t *a, size_t a_length, const uint8_t *b, size_t b_length, const uint8_t *c, size_t c_length,
                   uint8_t out[MD5_SIZE])
    {
        psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
        size_t out_length = 0;
        bool ok = psa_hash_setup(&op, PSA_ALG_MD5) == PSA_SUCCESS && psa_hash_update(&op, a, a_length) == PSA_SUCCESS &&
                  (b_length == 0 || psa_hash_update(&op, b, b_length) == PSA_SUCCESS) &&
                  (c_length == 0 || psa_hash_update(&op, c, c_length) == PSA_SUCCESS) &&
                  psa_hash_finish(&op, out, MD5_SIZE, &out_length) == PSA_SUCCESS && out_length == MD5_SIZE;
        if (!ok)
            psa_hash_abort(&op);
        return ok;
    }

    struct hmac_part_t
    {
        const uint8_t *data;
        size_t length;
    };

    // HMAC-MD5 of the concatenated parts (no copy of the packet on the stack)
    bool hmac_md5_parts(const uint8_t *secret, size_t secret_length, const hmac_part_t *parts, size_t count, uint8_t out[MD5_SIZE])
    {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
        psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_MD5));
        psa_key_id_t key = 0;
        if (psa_import_key(&attributes, secret, secret_length, &key) != PSA_SUCCESS)
            return false;
        psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
        bool ok = psa_mac_sign_setup(&op, key, PSA_ALG_HMAC(PSA_ALG_MD5)) == PSA_SUCCESS;
        for (size_t i = 0; ok && i < count; ++i)
            ok = parts[i].length == 0 || psa_mac_update(&op, parts[i].data, parts[i].length) == PSA_SUCCESS;
        size_t out_length = 0;
        ok = ok && psa_mac_sign_finish(&op, out, MD5_SIZE, &out_length) == PSA_SUCCESS && out_length == MD5_SIZE;
        if (!ok)
            psa_mac_abort(&op);
        psa_destroy_key(key);
        return ok;
    }

    bool hmac_md5(const uint8_t *secret, size_t secret_length, const uint8_t *data, size_t length, uint8_t out[MD5_SIZE])
    {
        const hmac_part_t part{data, length};
        return hmac_md5_parts(secret, secret_length, &part, 1, out);
    }

    bool equal(const uint8_t *a, const uint8_t *b, size_t length)
    {
        uint8_t diff = 0;
        for (size_t i = 0; i < length; ++i)
            diff |= static_cast<uint8_t>(a[i] ^ b[i]);
        return diff == 0;
    }

    void put16(uint8_t *p, size_t v)
    {
        p[0] = static_cast<uint8_t>(v >> 8);
        p[1] = static_cast<uint8_t>(v);
    }

    void put32(uint8_t *p, uint32_t v)
    {
        p[0] = static_cast<uint8_t>(v >> 24);
        p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >> 8);
        p[3] = static_cast<uint8_t>(v);
    }

    uint32_t get32(const uint8_t *p)
    {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
    }

    // Appends attributes to a packet under construction
    class writer_t
    {
    public:
        writer_t(uint8_t *out, size_t size) : out_(out), size_(size < RADIUS_PACKET_MAX ? size : RADIUS_PACKET_MAX) {}

        // Room for an attribute with a value of length bytes; nullptr if it does not fit (then every later call fails)
        uint8_t *reserve(uint8_t type, size_t length)
        {
            if (!ok_ || length > 253 || length_ + 2 + length > size_)
            {
                ok_ = false;
                return nullptr;
            }
            uint8_t *at = out_ + length_;
            at[0] = type;
            at[1] = static_cast<uint8_t>(2 + length);
            length_ += 2 + length;
            return at + 2;
        }

        void add(uint8_t type, const void *value, size_t length)
        {
            uint8_t *at = reserve(type, length);
            if (at != nullptr)
                memcpy(at, value, length);
        }

        void add32(uint8_t type, uint32_t value)
        {
            uint8_t v[4];
            put32(v, value);
            add(type, v, sizeof(v));
        }

        void add_text(uint8_t type, const char *text)
        {
            if (text == nullptr || text[0] == '\0')
                return;
            const size_t length = strlen(text);
            if (length > RADIUS_TEXT_MAX)
                ok_ = false;
            else
                add(type, text, length);
        }

        bool ok() const { return ok_; }
        size_t length() const { return length_; }

    private:
        uint8_t *out_;
        size_t size_;
        size_t length_ = RADIUS_HEADER;
        bool ok_ = true;
    };

    bool printable(const char *s, size_t length)
    {
        for (size_t i = 0; i < length; ++i)
            if (static_cast<unsigned char>(s[i]) < 0x20 || s[i] == 0x7F)
                return false;
        return true;
    }

    // Appends "name = value" to the summary: printable ASCII kept, anything else as '?', cut to fit
    void summary_add(char *summary, const char *name, const uint8_t *value, size_t length, bool quoted)
    {
        size_t at = strlen(summary);
        const size_t room = RADIUS_SUMMARY_SIZE - 1;
        const int n = snprintf(summary + at, RADIUS_SUMMARY_SIZE - at, "%s%s = %s", at > 0 ? ", " : "", name, quoted ? "\"" : "");
        if (n < 0)
            return;
        at = strlen(summary);
        for (size_t i = 0; i < length && at + (quoted ? 2 : 1) < room; ++i)
            summary[at++] = value[i] >= 0x20 && value[i] < 0x7F ? static_cast<char>(value[i]) : '?';
        if (quoted && at < room)
            summary[at++] = '"';
        summary[at] = '\0';
    }

    const char *service_type_name(uint32_t value)
    {
        switch (value)
        {
        case 1: return "Login-User";
        case 2: return "Framed-User";
        case 6: return "Administrative-User";
        case 7: return "NAS-Prompt-User";
        default: return nullptr;
        }
    }

    // "shell:priv-lvl=N", "priv-lvl=N" (also with '*'), N 0 to 15: N, else -1
    int avpair_privilege(const uint8_t *value, size_t length)
    {
        static constexpr char SHELL[] = "shell:";
        static constexpr char KEY[] = "priv-lvl";
        size_t at = 0;
        if (length >= sizeof(SHELL) - 1 && memcmp(value, SHELL, sizeof(SHELL) - 1) == 0)
            at = sizeof(SHELL) - 1;
        if (length - at < sizeof(KEY) - 1 + 2 || memcmp(value + at, KEY, sizeof(KEY) - 1) != 0)
            return -1;
        at += sizeof(KEY) - 1;
        if (value[at] != '=' && value[at] != '*')
            return -1;
        ++at;
        const size_t digits = length - at;
        if (digits < 1 || digits > 2)
            return -1;
        int level = 0;
        for (size_t i = at; i < length; ++i)
        {
            if (value[i] < '0' || value[i] > '9')
                return -1;
            level = level * 10 + (value[i] - '0');
        }
        return level <= 15 ? level : -1;
    }
} // namespace

bool radius_user_valid(const char *user, size_t length)
{
    return user != nullptr && length >= 1 && length <= RADIUS_USER_MAX && printable(user, length);
}

bool radius_password_valid(const char *password, size_t length)
{
    return password != nullptr && length >= 1 && length <= RADIUS_PASSWORD_MAX && memchr(password, '\0', length) == nullptr;
}

size_t radius_build_access_request(const radius_request_t &request, uint8_t identifier, const uint8_t authenticator[RADIUS_AUTHENTICATOR],
                                   const uint8_t *secret, size_t secret_length, uint8_t *out, size_t out_size)
{
    if (out_size < RADIUS_HEADER || secret_length == 0 || secret_length > RADIUS_SECRET_MAX ||
        !radius_user_valid(request.user, request.user_length) ||
        request.password == nullptr || request.password_length > RADIUS_PASSWORD_MAX)
        return 0;
    out[0] = RADIUS_ACCESS_REQUEST;
    out[1] = identifier;
    memcpy(out + 4, authenticator, RADIUS_AUTHENTICATOR);

    writer_t w(out, out_size);
    uint8_t *ma = w.reserve(RADIUS_MESSAGE_AUTHENTICATOR, MD5_SIZE); // first (draft-ietf-radext-deprecating-radius)
    w.add(RADIUS_USER_NAME, request.user, request.user_length);

    // User-Password (RFC 2865 5.2): padded with zeros to a multiple of 16 bytes (16 at least),
    // c(1) = p(1) xor MD5(S + RA), c(i) = p(i) xor MD5(S + c(i-1))
    const size_t padded = request.password_length == 0 ? 16 : (request.password_length + 15) / 16 * 16;
    uint8_t *hidden = w.reserve(RADIUS_USER_PASSWORD, padded);
    if (hidden != nullptr)
    {
        memset(hidden, 0, padded);
        memcpy(hidden, request.password, request.password_length);
        const uint8_t *previous = authenticator;
        for (size_t block = 0; block < padded; block += 16)
        {
            uint8_t b[MD5_SIZE];
            if (!md5_parts(secret, secret_length, previous, 16, nullptr, 0, b))
                return 0;
            for (size_t k = 0; k < 16; ++k)
                hidden[block + k] ^= b[k];
            memset(b, 0, sizeof(b));
            previous = hidden + block;
        }
    }
    w.add_text(RADIUS_NAS_IDENTIFIER, request.nas_identifier);
    if (request.nas_ip_set)
        w.add(RADIUS_NAS_IP_ADDRESS, request.nas_ip, 4);
    if (request.nas_ipv6_set)
        w.add(RADIUS_NAS_IPV6_ADDRESS, request.nas_ipv6, 16);
    if (request.nas_port_set)
        w.add32(RADIUS_NAS_PORT, request.nas_port);
    if (request.nas_port_type >= 0)
        w.add32(RADIUS_NAS_PORT_TYPE, static_cast<uint32_t>(request.nas_port_type));
    if (request.service_type != 0)
        w.add32(RADIUS_SERVICE_TYPE, request.service_type);
    w.add_text(RADIUS_NAS_PORT_ID, request.nas_port_id);
    w.add_text(RADIUS_CALLING_STATION_ID, request.calling_station_id);
    if (!w.ok())
        return 0;
    put16(out + 2, w.length());

    // Message-Authenticator: HMAC-MD5 of the whole packet with the attribute's value zero (RFC 3579 3.2)
    memset(ma, 0, MD5_SIZE);
    uint8_t mac[MD5_SIZE];
    if (!hmac_md5(secret, secret_length, out, w.length(), mac))
        return 0;
    memcpy(ma, mac, MD5_SIZE);
    return w.length();
}

bool radius_parse_required(const char *text, radius_required_t &out)
{
    out = radius_required_t{};
    if (text == nullptr || text[0] == '\0')
        return true;
    const char *eq = strchr(text, '=');
    if (eq == nullptr)
        return false;
    const size_t name_length = static_cast<size_t>(eq - text);
    const char *value = eq + 1;
    const size_t length = strlen(value);
    if (length == 0 || length > RADIUS_REQUIRED_MAX || !printable(value, length))
        return false;
    if (name_length == 12 && strncasecmp(text, "Cisco-AVPair", 12) == 0)
        out.type = RADIUS_VENDOR_SPECIFIC;
    else if (name_length == 5 && strncasecmp(text, "Class", 5) == 0)
        out.type = RADIUS_CLASS;
    else if (name_length == 9 && strncasecmp(text, "Filter-Id", 9) == 0)
        out.type = RADIUS_FILTER_ID;
    else
        return false;
    memcpy(out.value, value, length);
    out.value[length] = '\0';
    out.length = length;
    return true;
}

const char *radius_check_text(radius_check_t check)
{
    switch (check)
    {
    case radius_check_t::ok: return "valid";
    case radius_check_t::length: return "wrong length";
    case radius_check_t::code: return "not an answer to an Access-Request";
    case radius_check_t::identifier: return "another identifier";
    case radius_check_t::authenticator: return "wrong Response Authenticator (shared secret?)";
    case radius_check_t::attribute: return "malformed attributes";
    case radius_check_t::no_message_authenticator: return "no Message-Authenticator";
    case radius_check_t::message_authenticator: return "wrong Message-Authenticator";
    }
    return "invalid";
}

radius_check_t radius_parse_answer(const uint8_t *packet, size_t length, uint8_t identifier, const uint8_t request_authenticator[RADIUS_AUTHENTICATOR],
                                   const uint8_t *secret, size_t secret_length, bool require_message_authenticator,
                                   const radius_required_t &required, radius_answer_t &out)
{
    if (length < RADIUS_HEADER)
        return radius_check_t::length;
    const size_t declared = (static_cast<size_t>(packet[2]) << 8) | packet[3];
    if (declared < RADIUS_HEADER || declared > RADIUS_PACKET_MAX || declared > length)
        return radius_check_t::length;
    const uint8_t code = packet[0];
    if (code != RADIUS_ACCESS_ACCEPT && code != RADIUS_ACCESS_REJECT && code != RADIUS_ACCESS_CHALLENGE)
        return radius_check_t::code;
    if (packet[1] != identifier)
        return radius_check_t::identifier;

    // Response Authenticator = MD5(Code + Identifier + Length + Request Authenticator + Attributes + Secret)
    uint8_t expected[MD5_SIZE];
    {
        psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
        size_t n = 0;
        const bool ok = psa_hash_setup(&op, PSA_ALG_MD5) == PSA_SUCCESS && psa_hash_update(&op, packet, 4) == PSA_SUCCESS &&
                        psa_hash_update(&op, request_authenticator, RADIUS_AUTHENTICATOR) == PSA_SUCCESS &&
                        (declared == RADIUS_HEADER || psa_hash_update(&op, packet + RADIUS_HEADER, declared - RADIUS_HEADER) == PSA_SUCCESS) &&
                        psa_hash_update(&op, secret, secret_length) == PSA_SUCCESS &&
                        psa_hash_finish(&op, expected, MD5_SIZE, &n) == PSA_SUCCESS && n == MD5_SIZE;
        if (!ok)
        {
            psa_hash_abort(&op);
            return radius_check_t::authenticator;
        }
    }
    if (!equal(expected, packet + 4, MD5_SIZE))
        return radius_check_t::authenticator;

    // The attributes: within the packet; at most one Message-Authenticator
    size_t ma_at = 0;
    for (size_t at = RADIUS_HEADER; at < declared;)
    {
        if (declared - at < 2 || packet[at + 1] < 2 || packet[at + 1] > declared - at)
            return radius_check_t::attribute;
        if (packet[at] == RADIUS_MESSAGE_AUTHENTICATOR)
        {
            if (ma_at != 0 || packet[at + 1] != MA_ATTRIBUTE)
                return radius_check_t::attribute;
            ma_at = at;
        }
        at += packet[at + 1];
    }
    if (ma_at == 0 && require_message_authenticator)
        return radius_check_t::no_message_authenticator;
    if (ma_at != 0)
    {
        // HMAC-MD5 of the packet with the Request Authenticator in place of the Response Authenticator and the
        // attribute's value zero (RFC 3579 3.2)
        static constexpr uint8_t zero[MD5_SIZE] = {};
        const uint8_t *after = packet + ma_at + MA_ATTRIBUTE;
        const hmac_part_t parts[] = {{packet, 4},
                                     {request_authenticator, RADIUS_AUTHENTICATOR},
                                     {packet + RADIUS_HEADER, ma_at + 2 - RADIUS_HEADER},
                                     {zero, MD5_SIZE},
                                     {after, declared - (ma_at + MA_ATTRIBUTE)}};
        uint8_t mac[MD5_SIZE];
        if (!hmac_md5_parts(secret, secret_length, parts, sizeof(parts) / sizeof(parts[0]), mac) || !equal(mac, packet + ma_at + 2, MD5_SIZE))
            return radius_check_t::message_authenticator;
    }

    out = radius_answer_t{};
    out.code = code;
    out.message_authenticator = ma_at != 0;
    out.avpair_privilege = -1;
    out.required_found = required.type == 0;
    for (size_t at = RADIUS_HEADER; at < declared; at += packet[at + 1])
    {
        const uint8_t type = packet[at];
        const uint8_t *value = packet + at + 2;
        const size_t value_length = packet[at + 1] - 2u;
        if (type == RADIUS_SERVICE_TYPE && value_length == 4)
        {
            out.service_type = get32(value);
            const char *name = service_type_name(out.service_type);
            char number[12];
            snprintf(number, sizeof(number), "%lu", static_cast<unsigned long>(out.service_type));
            const char *shown = name != nullptr ? name : number;
            summary_add(out.summary, "Service-Type", reinterpret_cast<const uint8_t *>(shown), strlen(shown), false);
        }
        else if (type == RADIUS_CLASS || type == RADIUS_FILTER_ID || type == RADIUS_REPLY_MESSAGE)
        {
            summary_add(out.summary, type == RADIUS_CLASS ? "Class" : type == RADIUS_FILTER_ID ? "Filter-Id" : "Reply-Message",
                        value, value_length, true);
            if (required.type == type && value_length == required.length && memcmp(value, required.value, value_length) == 0)
                out.required_found = true;
        }
        else if (type == RADIUS_VENDOR_SPECIFIC && value_length >= 4 && get32(value) == RADIUS_VENDOR_CISCO)
        {
            // Cisco: vendor type 1 (Cisco-AVPair), one or more sub-attributes
            for (size_t s = 4; s + 2 <= value_length;)
            {
                const uint8_t sub_type = value[s];
                const size_t sub_length = value[s + 1];
                if (sub_length < 2 || sub_length > value_length - s)
                    break;
                const uint8_t *sub = value + s + 2;
                const size_t sub_value_length = sub_length - 2;
                if (sub_type == RADIUS_CISCO_AVPAIR)
                {
                    summary_add(out.summary, "Cisco-AVPair", sub, sub_value_length, true);
                    const int level = avpair_privilege(sub, sub_value_length);
                    if (level >= 0 && (out.avpair_privilege < 0 || level < out.avpair_privilege))
                        out.avpair_privilege = level;
                    if (required.type == RADIUS_VENDOR_SPECIFIC && sub_value_length == required.length &&
                        memcmp(sub, required.value, sub_value_length) == 0)
                        out.required_found = true;
                }
                s += sub_length;
            }
        }
    }
    return radius_check_t::ok;
}

int radius_privilege(const radius_answer_t &answer)
{
    if (answer.avpair_privilege >= 0)
        return answer.avpair_privilege;
    return answer.service_type == RADIUS_SERVICE_ADMINISTRATIVE ? 15 : 1;
}
