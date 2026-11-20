// ESP32 Time Server host tests (from ESP32-WebTherm): no two items the firmware can write have the same 24-bit hash of
// ESP-IDF's NVS.
//
// ESP-IDF 6.1.1 Page::mLoadEntryTable() (components/nvs_flash/src/nvs_page.cpp) erased, at a start, an earlier item of
// the active page whose 24-bit hash (Item::calculateCrc32WithoutValue() & 0xffffff: CRC-32 over the namespace index,
// the 16 key bytes and the chunk index) equals that of a later item, without comparing namespace index, key and chunk
// index. The build corrects that (tools/cmake/nvs_flash_fix.cmake); this test is the second line of defence: for every
// order in which the namespaces may have got their indexes (1, 2, ... in the order of first use), every item the
// firmware can write (the namespace entries, every number, string and blob index, and every chunk index NVS can give a
// data chunk of a blob: 0 to 126 of version 0 and 0x80 to 0xFE of version 1) has a hash of its own.
//
// WebTherm's test walks every order (5 or 6 namespaces). This firmware has 9 with the marks, which makes 362880 orders
// of about 4900 items: too slow under the sanitizers. The test here is the same check without the walk: two items can be on one NVS
// together exactly when they are namespace entries, or items of one namespace at the same index, or items of two
// namespaces at two different indexes (an order exists that gives them these indexes). So every namespace's items are
// hashed at every index 1 ... N and a pair with equal hash and such a placement fails the test. Item counts of one
// order are printed as in WebTherm.
//
// The key table below is checked against the firmware sources: every `constexpr char <name with NVS>[] = "..."` in
// main/ (the convention of every NVS name of the firmware) must be in the table, and every name of the table must be
// in the sources; every record_mark_set/check/clear() call of main/ must name a record of the marked list, which gives
// the keys of the mark namespace (sys/record_mark.h). A new key, namespace or mark thus fails this test until it is
// added here.
#define TEST_NAME "nvs_hash_test"
#include "check.h"

#include "auth/totp_store.h"
#include "sys/record_mark.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // CRC-32 as ESP-IDF's esp_rom_crc32_le(crc, data, length) (reflected, polynomial 0xEDB88320, crc inverted before
    // and after), chainable as the NVS code uses it
    uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t length)
    {
        crc = ~crc;
        for (size_t i = 0; i < length; ++i)
        {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    // Item::calculateCrc32WithoutValue() & 0xffffff (nvs_types.cpp, nvs_item_hash_list.cpp)
    uint32_t hash24(uint8_t ns, const std::string &key, uint8_t chunk)
    {
        uint8_t key_bytes[16] = {};
        memcpy(key_bytes, key.data(), std::min<size_t>(key.size(), 16));
        uint32_t crc = crc32_le(0xffffffffu, &ns, 1);
        crc = crc32_le(crc, key_bytes, sizeof(key_bytes));
        crc = crc32_le(crc, &chunk, 1);
        return crc & 0xffffffu;
    }

    enum class kind_t
    {
        value, // number or string: one item, chunk index 0xFF
        blob,  // blob: its index (0xFF) and data chunks of both versions
    };

    struct key_t
    {
        std::string key;
        kind_t kind;
    };

    struct namespace_t
    {
        std::string name;
        std::vector<key_t> keys;
    };

    // The records with a presence mark (sys/record_mark.h): namespace and key ("t*": the token keys)
    const std::vector<std::pair<std::string, std::string>> MARKED = {
        {"webauth", "pw"},    // web_auth.cpp
        {"webauth", "allow"}, // web_auth.cpp
        {"webcfg", "saved"},  // web_settings.cpp
        {"totp", "t*"},       // totp_store.cpp
    };

    // Every NVS item of the firmware, by namespace. The source of each line is named.
    std::vector<namespace_t> firmware_items()
    {
        std::vector<namespace_t> table = {
            {"webcfg", {{"saved", kind_t::blob}, {"oneshot", kind_t::blob}}}, // web_settings.cpp
            {"webauth", {{"pw", kind_t::blob}, {"allow", kind_t::blob}}},     // web_auth.cpp
            {"webtls", {{"key", kind_t::blob}, {"crt", kind_t::blob}, {"name", kind_t::value}, {"mode", kind_t::value},
                        {"opair2", kind_t::blob}, {"opair", kind_t::blob}, {"ocrt", kind_t::blob}, {"okey", kind_t::blob}}}, // web_tls.cpp
            {"gnss_state", {{"id_type", kind_t::value}, {"id_value", kind_t::value}, {"max_baud", kind_t::value},
                            {"initial_baud", kind_t::value}, {"no_sig_rcv", kind_t::value}}}, // main.cpp (no_sig_rcv: only erased)
            {"isr_latency", {{"ns", kind_t::value}, {"fp", kind_t::value}}},                 // main.cpp
            {"ptp_latency", {{"ns", kind_t::value}, {"fp", kind_t::value}}},                 // main.cpp
            {"restart_evt", {{"pending", kind_t::value}}},                                   // main.cpp
            {TOTP_NVS_NAMESPACE, {}},                                                        // totp_store.cpp
        };
        // The tokens "t0" ... "t8" (totp_store.cpp, key_of(): "t%u")
        for (size_t i = 0; i < TOTP_TOKENS_MAX; ++i)
            table.back().keys.push_back({"t" + std::to_string(i), kind_t::blob});
        // The marks: u64 items "<namespace>/<key>" (record_mark_key)
        namespace_t marks{RECORD_MARK_NVS_NAMESPACE, {}};
        for (const auto &[ns, key] : MARKED)
            for (size_t i = 0; i < (key == "t*" ? TOTP_TOKENS_MAX : 1); ++i)
            {
                char mark[RECORD_MARK_KEY_SIZE];
                const std::string record_key = key == "t*" ? "t" + std::to_string(i) : key;
                CHECK(record_mark_key(ns.c_str(), record_key.c_str(), mark));
                marks.keys.push_back({mark, kind_t::value});
            }
        table.push_back(marks);
        return table;
    }

    // The names made at run time, not as constants: checked separately
    bool generated(const std::string &key)
    {
        return std::regex_match(key, std::regex("t[0-9]+")) || key.find('/') != std::string::npos;
    }

    std::string read_file(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    std::filesystem::path main_dir()
    {
        // This file is testing/host/nvs_hash_test.cpp; run_tests.ps1 compiles it by its absolute path
        return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "main";
    }

    void test_hash_formula()
    {
        const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        CHECK(crc32_le(0, check, sizeof(check)) == 0xCBF43926u);           // the CRC-32 check value
        CHECK(crc32_le(0xffffffffu, check, sizeof(check)) == 0xD202D277u); // as zlib.crc32(data, 0xffffffff)
        // Measured on a WebTherm board and on this project's test board (ESP-IDF 6.1.1 without the correction): of two
        // U8 items "zha" and "zcv5bii" of namespace index 2 on the active page, the start erased "zha". Both have this
        // hash.
        CHECK(hash24(2, "zha", 0xFF) == 0x26e618u);
        CHECK(hash24(2, "zcv5bii", 0xFF) == 0x26e618u);
        CHECK(hash24(2, "zha", 0xFF) != hash24(3, "zha", 0xFF));
    }

    // The key table is the firmware's: every NVS name constant of main/ is in it, every name of it is in main/
    void test_table_matches_sources()
    {
        const std::filesystem::path dir = main_dir();
        CHECK(std::filesystem::is_directory(dir));
        const std::regex constant(R"(constexpr\s+char\s+(\w*NVS\w*)\s*\[\]\s*=\s*"([^"]*)\")");
        std::set<std::string> source_namespaces, source_keys;
        std::map<std::string, std::map<std::string, std::string>> file_constants; // file -> name -> value
        std::map<std::string, std::string> constants;                             // name -> value (all files)
        std::vector<std::pair<std::string, std::string>> mark_calls;               // file, the call's text
        const std::regex mark_call(R"(record_mark_(?:set|check|clear)\s*\(\s*(\w+)\s*,\s*(\w+))");
        bool totp_pattern = false;
        size_t files = 0;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(dir))
        {
            const std::string ext = entry.path().extension().string();
            if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h") ||
                entry.path().string().find("third_party") != std::string::npos)
                continue;
            ++files;
            const std::string text = read_file(entry.path());
            for (std::sregex_iterator m(text.begin(), text.end(), constant), end; m != end; ++m)
            {
                const std::string name = (*m)[1].str();
                const std::string value = (*m)[2].str();
                const bool is_namespace = name.size() >= 9 && name.compare(name.size() - 9, 9, "NAMESPACE") == 0;
                (is_namespace ? source_namespaces : source_keys).insert(value);
                file_constants[entry.path().string()][name] = value;
                constants[name] = value;
            }
            for (std::sregex_iterator m(text.begin(), text.end(), mark_call), end; m != end; ++m)
                if (entry.path().filename() != "record_mark.cpp") // its own definitions
                    mark_calls.push_back({entry.path().string(), (*m)[1].str() + " " + (*m)[2].str()});
            if (entry.path().filename() == "totp_store.cpp")
                totp_pattern = text.find("\"t%u\"") != std::string::npos;
        }
        CHECK(files > 50);
        CHECK(totp_pattern); // the token keys are still "t<slot>"

        std::set<std::string> table_namespaces, table_keys;
        for (const namespace_t &ns : firmware_items())
        {
            table_namespaces.insert(ns.name);
            for (const key_t &k : ns.keys)
                if (!generated(k.key))
                    table_keys.insert(k.key);
        }
        for (const std::string &n : source_namespaces)
            if (table_namespaces.count(n) == 0)
            {
                fprintf(stderr, "NVS namespace \"%s\" of main/ is not in the table of nvs_hash_test.cpp: add it\n", n.c_str());
                CHECK(false);
            }
        for (const std::string &k : source_keys)
            if (table_keys.count(k) == 0)
            {
                fprintf(stderr, "NVS key \"%s\" of main/ is not in the table of nvs_hash_test.cpp: add it\n", k.c_str());
                CHECK(false);
            }
        for (const std::string &n : table_namespaces)
            CHECK(source_namespaces.count(n) == 1);
        for (const std::string &k : table_keys)
            CHECK(source_keys.count(k) == 1);

        // Every record_mark_*() call names a marked record: (namespace constant, key constant), or the token key
        // variable "key" of totp_store.cpp
        std::set<std::pair<std::string, std::string>> called;
        for (const auto &[file, call] : mark_calls)
        {
            const std::string ns_name = call.substr(0, call.find(' '));
            const std::string key_name = call.substr(call.find(' ') + 1);
            const auto &local = file_constants[file];
            auto value = [&](const std::string &name) {
                if (local.count(name))
                    return local.at(name);
                return constants.count(name) ? constants.at(name) : std::string();
            };
            const std::string ns = value(ns_name);
            const bool token = key_name == "key" && file.find("totp_store.cpp") != std::string::npos;
            const std::string key = token ? std::string("t*") : value(key_name);
            const bool known = std::find(MARKED.begin(), MARKED.end(), std::make_pair(ns, key)) != MARKED.end();
            if (!known)
                fprintf(stderr, "record_mark call (%s) in %s: not in the marked list of nvs_hash_test.cpp\n", call.c_str(), file.c_str());
            CHECK(known);
            called.insert({ns, key});
        }
        CHECK(called.size() == MARKED.size()); // and every marked record is used
    }

    struct placed_t
    {
        uint32_t hash;
        int ns;      // number of the namespace in the table; -1 for the namespace entries (namespace index 0)
        int index;   // namespace index the items are placed at (0 for the namespace entries)
        std::string item;
    };

    // Can a and b be on one NVS together (see the header)? Different items of the table are assumed.
    bool together(const placed_t &a, const placed_t &b)
    {
        if (a.ns < 0 || b.ns < 0)
            return true;                  // the namespace entries are on every NVS
        if (a.ns == b.ns)
            return a.index == b.index;    // one namespace has one index
        return a.index != b.index;        // two namespaces have two indexes
    }

    // For every order of the namespace indexes: no two different items with the same hash
    void test_no_equal_hashes()
    {
        const std::vector<namespace_t> table = firmware_items();
        const int count = static_cast<int>(table.size());
        std::vector<placed_t> placed;
        size_t per_order = 0;
        auto add_items = [&](int n, int index) {
            auto add = [&](const std::string &key, uint8_t chunk) {
                placed.push_back({hash24(static_cast<uint8_t>(index), key, chunk), n, index,
                                  table[n].name + "@" + std::to_string(index) + "/" + key + "/" + std::to_string(chunk)});
            };
            for (const key_t &k : table[n].keys)
            {
                add(k.key, 0xFF); // the value, or the index of a blob
                if (k.kind == kind_t::blob)
                    for (unsigned c = 0; c < 127; ++c)
                    {
                        add(k.key, static_cast<uint8_t>(c));        // version 0
                        add(k.key, static_cast<uint8_t>(0x80 + c)); // version 1
                    }
            }
        };
        for (int n = 0; n < count; ++n)
        {
            placed.push_back({hash24(0, table[n].name, 0xFF), -1, 0, "namespace entry " + table[n].name}); // a U8 in namespace 0
            const size_t before = placed.size();
            add_items(n, 1);
            per_order += placed.size() - before + 1;
            for (int index = 2; index <= count; ++index)
                add_items(n, index);
        }
        std::sort(placed.begin(), placed.end(), [](const placed_t &a, const placed_t &b) { return a.hash < b.hash; });
        size_t equal = 0, apart = 0;
        for (size_t i = 0; i < placed.size();)
        {
            size_t j = i;
            while (j < placed.size() && placed[j].hash == placed[i].hash)
                ++j;
            for (size_t a = i; a < j; ++a)
                for (size_t b = a + 1; b < j; ++b)
                {
                    if (together(placed[a], placed[b]))
                    {
                        if (equal < 10)
                            fprintf(stderr, "equal hash %06x: %s and %s\n", static_cast<unsigned>(placed[a].hash),
                                    placed[a].item.c_str(), placed[b].item.c_str());
                        ++equal;
                    }
                    else
                        ++apart; // equal hash, but never on one NVS together
                }
            i = j;
        }
        size_t orders = 1;
        for (int i = 2; i <= count; ++i)
            orders *= static_cast<size_t>(i);
        printf("  %zu orders of %d namespace indexes, %zu items each (%zu placed), %zu equal hashes (%zu pairs that are "
               "never together)\n",
               orders, count, per_order, placed.size(), equal, apart);
        CHECK(per_order > 4000);
        CHECK(equal == 0);

        // The check itself: a pair that is together in an order is found (the two items of the board, made one
        // namespace), one that never is is not counted
        const placed_t x{0x26e618u, 0, 2, "zha"}, y{0x26e618u, 0, 2, "zcv5bii"}, z{0x26e618u, 0, 3, "zcv5bii"},
            w{0x26e618u, 1, 3, "other"}, v{0x26e618u, 1, 2, "other"}, e{0x26e618u, -1, 0, "entry"};
        CHECK(together(x, y));
        CHECK(!together(x, z));
        CHECK(together(x, w));
        CHECK(!together(x, v));
        CHECK(together(x, e) && together(e, v));
    }
} // namespace

void run_tests()
{
    test_hash_formula();
    test_table_matches_sources();
    test_no_equal_hashes();
}

void fuzz_one(const uint8_t *, size_t)
{
}
