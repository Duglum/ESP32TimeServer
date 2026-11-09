// ESP32 Time Server host tests (from ESP32-ConsoleServer): TOTP (main/auth/totp) and the QR codes of its URI (main/third_party/qrcodegen)
//
// Known answers: base32 of RFC 4648 10, HOTP of RFC 4226 appendix D, TOTP (SHA-1) of RFC 6238 appendix B.
#define TEST_NAME "totp_test"

#include "check.h"

#include "auth/totp.h"

#include <string>

extern "C"
{
#include "psa/crypto.h"
#include "third_party/qrcodegen/qrcodegen.h"
}

namespace
{
    const uint8_t RFC_SECRET[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};

    std::string encode(const char *text)
    {
        char out[64];
        totp_base32_encode(reinterpret_cast<const uint8_t *>(text), strlen(text), out, sizeof(out));
        return out;
    }

    std::string decode(const char *text, bool *ok = nullptr)
    {
        uint8_t out[64];
        size_t length = 0;
        const bool decoded = totp_base32_decode(text, out, sizeof(out), length);
        if (ok != nullptr)
            *ok = decoded;
        return decoded ? std::string(reinterpret_cast<const char *>(out), length) : std::string("<error>");
    }

    std::string code_at(int64_t time_s)
    {
        uint32_t code = 0;
        CHECK(totp_hotp(RFC_SECRET, sizeof(RFC_SECRET), static_cast<uint64_t>(time_s) / TOTP_PERIOD_S, code));
        char text[TOTP_DIGITS + 1];
        totp_code_text(code, text);
        return text;
    }

    void test_base32()
    {
        CHECK(encode("") == "" && encode("f") == "MY" && encode("fo") == "MZXQ" && encode("foo") == "MZXW6");
        CHECK(encode("foob") == "MZXW6YQ" && encode("fooba") == "MZXW6YTB" && encode("foobar") == "MZXW6YTBOI");
        CHECK(decode("MZXW6YTBOI") == "foobar" && decode("mzxw6ytboi") == "foobar" && decode("MZXW6YQ=") == "foob");
        CHECK(decode("MZXW 6YTB-OI") == "foobar" && decode("") == "");
        bool ok = true;
        decode("MZXW1", &ok);
        CHECK(!ok);
        decode("MZ8W", &ok);
        CHECK(!ok);
        decode("MZ!W", &ok);
        CHECK(!ok);
        // 20 bytes: 32 characters; too small an output
        char key[TOTP_BASE32_SIZE];
        CHECK(totp_base32_encode(RFC_SECRET, sizeof(RFC_SECRET), key, sizeof(key)) == 32);
        CHECK_STR(key, "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ");
        CHECK(totp_base32_encode(RFC_SECRET, sizeof(RFC_SECRET), key, 32) == 0 && key[0] == '\0');
        uint8_t small[3];
        size_t length = 0;
        CHECK(!totp_base32_decode("MZXW6YTB", small, sizeof(small), length));
    }

    void test_hotp_totp()
    {
        static const uint32_t HOTP[] = {755224, 287082, 359152, 969429, 338314, 254676, 287922, 162583, 399871, 520489};
        for (uint64_t c = 0; c < 10; ++c)
        {
            uint32_t code = 0;
            CHECK(totp_hotp(RFC_SECRET, sizeof(RFC_SECRET), c, code) && code == HOTP[c]);
        }
        CHECK(code_at(59) == "287082" && code_at(1111111109) == "081804" && code_at(1111111111) == "050471");
        CHECK(code_at(1234567890) == "005924" && code_at(2000000000) == "279037" && code_at(20000000000LL) == "353130");

        uint64_t step = 0;
        // the step of now, one before, one after; not two away
        CHECK(totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 59, "287082", 0, step) && step == 1);
        CHECK(totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 89, "287082", 0, step) && step == 1);
        CHECK(totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 31, "287082", 0, step) && step == 1);
        CHECK(totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 59, code_at(60).c_str(), 0, step) && step == 2);
        CHECK(!totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 119, "287082", 0, step) && step == 0);
        CHECK(!totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 1111111111 + 61, "050471", 0, step));
        // each code once: not for the step used last or an earlier one
        CHECK(!totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 59, "287082", 1, step));
        CHECK(!totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 89, "287082", 2, step));
        CHECK(totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 89, code_at(60).c_str(), 1, step) && step == 2);
        // the form of the code
        CHECK(totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 59, "287 082", 0, step));
        for (const char *bad : {"28708", "2870821", "28708a", "", "287-082", "+287082"})
            CHECK(!totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 59, bad, 0, step));
        CHECK(!totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 59, nullptr, 0, step));
        // no time yet (before 1970 + 30 s), no key
        CHECK(!totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 29, code_at(29).c_str(), 0, step));
        CHECK(!totp_verify(RFC_SECRET, 0, 59, "287082", 0, step));
        // a wrong code at a time of today
        const int64_t now = 1794220000;
        std::string wrong = code_at(now);
        wrong[0] = wrong[0] == '9' ? '0' : static_cast<char>(wrong[0] + 1);
        CHECK(totp_verify(RFC_SECRET, sizeof(RFC_SECRET), now, code_at(now).c_str(), 0, step) && step == static_cast<uint64_t>(now) / 30);
        CHECK(!totp_verify(RFC_SECRET, sizeof(RFC_SECRET), now, wrong.c_str(), 0, step));
    }

    void test_uri_and_qr()
    {
        char uri[TOTP_URI_SIZE];
        CHECK(totp_uri("esp32consoleserver", "jdoe@corp", RFC_SECRET, sizeof(RFC_SECRET), uri, sizeof(uri)));
        CHECK_STR(uri, "otpauth://totp/esp32consoleserver:jdoe%40corp?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=esp32consoleserver"
                       "&algorithm=SHA1&digits=6&period=30");
        CHECK(totp_uri("cs", "a b/ü", RFC_SECRET, sizeof(RFC_SECRET), uri, sizeof(uri)) && strstr(uri, "totp/cs:a%20b%2F%C3%BC?") != nullptr);
        CHECK(!totp_uri("cs", "jdoe", RFC_SECRET, sizeof(RFC_SECRET), uri, 60) && uri[0] == '\0');
        // the longest: a host name of 32 and a user name of 64 characters, every one percent-encoded
        const std::string host(32, 'h');
        const std::string user(64, '@');
        CHECK(totp_uri(host.c_str(), user.c_str(), RFC_SECRET, sizeof(RFC_SECRET), uri, sizeof(uri)));
        // ... in a QR code of version 15 at most, error correction M (as the web server makes it)
        static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
        static uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
        CHECK(qrcodegen_encodeText(uri, temp, qr, qrcodegen_Ecc_MEDIUM, 1, 15, qrcodegen_Mask_AUTO, true));
        const int size = qrcodegen_getSize(qr);
        CHECK(size >= 21 && size <= 21 + 4 * 14 && (size - 17) % 4 == 0);
        // the finder patterns: dark corners, light separators
        CHECK(qrcodegen_getModule(qr, 0, 0) && qrcodegen_getModule(qr, size - 1, 0) && qrcodegen_getModule(qr, 0, size - 1));
        CHECK(!qrcodegen_getModule(qr, 7, 0) && !qrcodegen_getModule(qr, size - 8, 0) && !qrcodegen_getModule(qr, 0, size - 8));
        CHECK(qrcodegen_getModule(qr, 3, 3) && !qrcodegen_getModule(qr, 1, 1));
        // a short URI: version 6 or so
        CHECK(totp_uri("cs", "admin", RFC_SECRET, sizeof(RFC_SECRET), uri, sizeof(uri)));
        CHECK(qrcodegen_encodeText(uri, temp, qr, qrcodegen_Ecc_MEDIUM, 1, 15, qrcodegen_Mask_AUTO, true) && qrcodegen_getSize(qr) <= 45);
    }

    void test_recovery()
    {
        const uint8_t random[7] = {0x00, 0x44, 0x32, 0x14, 0xC7, 0x42, 0x54};
        char text[TOTP_RECOVERY_TEXT_SIZE];
        totp_recovery_text(random, text);
        CHECK(strlen(text) == 11 && text[5] == '-');
        // 50 bits: the top 6 bits of the 7 bytes are dropped; the alphabet is lower case base32
        const uint8_t ones[7] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        totp_recovery_text(ones, text);
        CHECK_STR(text, "77777-77777");
        const uint8_t zeros[7] = {};
        totp_recovery_text(zeros, text);
        CHECK_STR(text, "aaaaa-aaaaa");
        char normalised[11];
        CHECK(totp_recovery_normalise("ABCDE-FGHIJ", normalised) && strcmp(normalised, "abcdefghij") == 0);
        CHECK(totp_recovery_normalise("abcde fghij", normalised) && totp_recovery_normalise("abcdefghij", normalised));
        for (const char *bad : {"abcde-fghi", "abcde-fghij2", "abcde-fghi1", "abcde-fghi0", "abcde_fghij", ""})
            CHECK(!totp_recovery_normalise(bad, normalised));

        const uint8_t salt[TOTP_SALT_SIZE] = {1, 2, 3, 4, 5, 6, 7, 8};
        uint8_t hashes[TOTP_RECOVERY_CODES][TOTP_RECOVERY_HASH];
        char codes[TOTP_RECOVERY_CODES][TOTP_RECOVERY_TEXT_SIZE];
        for (size_t i = 0; i < TOTP_RECOVERY_CODES; ++i)
        {
            uint8_t r[7] = {static_cast<uint8_t>(i), 0x10, 0x20, 0x30, 0x40, 0x50, static_cast<uint8_t>(0x60 + i)};
            totp_recovery_text(r, codes[i]);
            CHECK(totp_recovery_normalise(codes[i], normalised) && totp_recovery_hash(salt, normalised, hashes[i]));
        }
        for (size_t i = 0; i < TOTP_RECOVERY_CODES; ++i)
            CHECK(totp_recovery_find(salt, hashes, codes[i]) == static_cast<int>(i));
        std::string upper(codes[3]);
        for (char &c : upper)
            c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        CHECK(totp_recovery_find(salt, hashes, upper.c_str()) == 3);
        memset(hashes[3], 0, TOTP_RECOVERY_HASH); // used
        CHECK(totp_recovery_find(salt, hashes, codes[3]) == -1);
        CHECK(totp_recovery_find(salt, hashes, "zzzzz-zzzzz") == -1 && totp_recovery_find(salt, hashes, "123456") == -1);
        CHECK(totp_recovery_find(salt, hashes, nullptr) == -1);
        const uint8_t other_salt[TOTP_SALT_SIZE] = {9};
        CHECK(totp_recovery_find(other_salt, hashes, codes[4]) == -1);
    }
} // namespace

void run_tests()
{
    CHECK(psa_crypto_init() == PSA_SUCCESS);
    test_base32();
    test_hotp_totp();
    test_uri_and_qr();
    test_recovery();
}

// Fuzz target: base32 both ways, a code typed by the user, a recovery code, and a URI with the input as the user name
void fuzz_one(const uint8_t *data, size_t size)
{
    static bool init = psa_crypto_init() == PSA_SUCCESS;
    CHECK(init);
    const std::string text(reinterpret_cast<const char *>(data), strnlen(reinterpret_cast<const char *>(data), size));
    uint8_t bytes[64];
    size_t length = 0;
    if (totp_base32_decode(text.c_str(), bytes, sizeof(bytes), length))
    {
        char again[128];
        CHECK(length <= sizeof(bytes) && totp_base32_encode(bytes, length, again, sizeof(again)) <= sizeof(again) - 1);
        uint8_t back[64];
        size_t back_length = 0;
        CHECK(totp_base32_decode(again, back, sizeof(back), back_length) && back_length == length && memcmp(back, bytes, length) == 0);
    }
    uint64_t step = 0;
    if (totp_verify(RFC_SECRET, sizeof(RFC_SECRET), 1794220000, text.c_str(), 0, step))
        CHECK(step + 1 >= 1794220000 / 30 && step <= 1794220000 / 30 + 1);
    char normalised[11];
    if (totp_recovery_normalise(text.c_str(), normalised))
        CHECK(strlen(normalised) == 10);
    char uri[TOTP_URI_SIZE];
    if (totp_uri("cs", text.c_str(), RFC_SECRET, sizeof(RFC_SECRET), uri, sizeof(uri)))
        CHECK(strlen(uri) < sizeof(uri) && strncmp(uri, "otpauth://totp/cs:", 18) == 0);
}
