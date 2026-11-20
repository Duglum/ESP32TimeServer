// ESP32 Time Server host tests: the stored form of the own web certificate, the NVS room check and the rule which
// pair is stored after updates and downgrades between the three layouts (main/web/web_tls_pair.cpp)
#define TEST_NAME "web_tls_pair_test"
#include "check.h"

#include "web/web_tls_pair.h"

#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    bool split(const std::vector<uint8_t> &blob, std::string &cert, std::string &key)
    {
        const char *c = nullptr;
        const char *k = nullptr;
        if (!web_tls_pair_split(blob.data(), blob.size(), c, k))
            return false;
        cert = c;
        key = k;
        return true;
    }

    std::vector<uint8_t> bytes(const char *text, size_t size)
    {
        return std::vector<uint8_t>(reinterpret_cast<const uint8_t *>(text), reinterpret_cast<const uint8_t *>(text) + size);
    }

    void test_pack_split()
    {
        std::vector<uint8_t> out(32, 0xAA);
        CHECK(web_tls_pair_pack("CERT", "KEY", out.data(), out.size()) == 9);
        CHECK(memcmp(out.data(), "CERT\0KEY\0", 9) == 0);
        CHECK(out[9] == 0xAA);
        out.resize(9);
        std::string cert, key;
        CHECK(split(out, cert, key));
        CHECK_STR(cert.c_str(), "CERT");
        CHECK_STR(key.c_str(), "KEY");
        // Too small, empty texts, null
        std::vector<uint8_t> small(8);
        CHECK(web_tls_pair_pack("CERT", "KEY", small.data(), small.size()) == 0);
        CHECK(web_tls_pair_pack("", "KEY", out.data(), out.size()) == 0);
        CHECK(web_tls_pair_pack("CERT", "", out.data(), out.size()) == 0);
        CHECK(web_tls_pair_pack(nullptr, "KEY", out.data(), out.size()) == 0);
        CHECK(web_tls_pair_pack("CERT", "KEY", nullptr, 100) == 0);
        CHECK(web_tls_pair_size(8192, 4096) == 12290);

        // The largest pair: round trip
        std::string big_cert(8192, 'c'), big_key(4096, 'k');
        std::vector<uint8_t> big(web_tls_pair_size(big_cert.size(), big_key.size()));
        CHECK(web_tls_pair_pack(big_cert.c_str(), big_key.c_str(), big.data(), big.size()) == big.size());
        CHECK(split(big, cert, key));
        CHECK(cert == big_cert && key == big_key);
    }

    void test_split_refused()
    {
        std::string cert, key;
        CHECK(!split(bytes("CERT\0KEY", 8), cert, key));         // no NUL at the end
        CHECK(!split(bytes("CERT\0", 5), cert, key));             // no key
        CHECK(!split(bytes("CERT\0\0", 6), cert, key));           // empty key
        CHECK(!split(bytes("\0KEY\0", 5), cert, key));            // empty chain
        CHECK(!split(bytes("CERT\0K\0Y\0", 9), cert, key));       // a third text
        CHECK(!split(bytes("CERTKEY\0", 8), cert, key));          // one text only
        CHECK(!split(bytes("", 0), cert, key));
        CHECK(split(bytes("C\0K\0", 4), cert, key));
        const char *c = "x";
        const char *k = "y";
        CHECK(!web_tls_pair_split(nullptr, 4, c, k) && c == nullptr && k == nullptr);
    }

    void test_entries()
    {
        // Measured on the board (NVS listings and used entries, 2026-11-19): a blob takes at most the estimate and at
        // least the lower bound. ocrt 6001 bytes in 3 chunks: 192 entries; okey 1709 bytes in 2 chunks: 57; ocrt
        // 1706 bytes in 1 chunk: 56; crt 472 bytes: 17; key 121 bytes: 6; opair 2387 bytes in 2 chunks: 78; opair
        // 3415 bytes in 2 chunks: 110; opair 12290 bytes: 390 (used entries before and after).
        const struct
        {
            size_t size, measured;
        } stored[] = {{6001, 192}, {1709, 57}, {1706, 56}, {472, 17}, {121, 6}, {2387, 78}, {3415, 110}, {12290, 390}};
        for (const auto &s : stored)
        {
            CHECK(web_tls_nvs_entries_max(s.size) >= s.measured);
            CHECK(web_tls_nvs_entries_min(s.size) <= s.measured);
        }
        CHECK(web_tls_nvs_entries_max(12290) == 385 + 4 + 5 + 1);
        CHECK(web_tls_nvs_entries_min(12290) == 387);
        CHECK(web_tls_nvs_entries_max(6996) == 219 + 2 + 3 + 1);
        // Worst case of the split: every chunk rounded up, one chunk more than size / 4000
        for (size_t size = 1; size <= 20000; ++size)
        {
            const size_t chunks = (size + 3999) / 4000 + 1;
            CHECK(web_tls_nvs_entries_max(size) >= (size + 31) / 32 + chunks - 1 + chunks + 1);
            CHECK(web_tls_nvs_entries_min(size) <= web_tls_nvs_entries_max(size));
        }
    }

    void test_room()
    {
        const size_t reserve = web_tls_nvs_entries_max(6996); // 225
        size_t needed = 0;
        // Old table with the stored state of the test board: 253 available, the TEST pair (ocrt 1706 + okey 1709)
        const size_t old = web_tls_nvs_entries_min(1706) + web_tls_nvs_entries_min(1709);
        CHECK(old == 112);
        // Chain 6000 + key 4096 (fix 8's case): refused before anything is written
        CHECK(!web_tls_nvs_room(253, web_tls_pair_size(6000, 4096), 0, old, reserve, needed));
        CHECK(needed == web_tls_nvs_entries_max(10098) - old + reserve);
        // The largest pair: refused
        CHECK(!web_tls_nvs_room(253, web_tls_pair_size(8192, 4096), 0, old, reserve, needed));
        // The TEST pair again in the new layout: fits
        CHECK(web_tls_nvs_room(253, web_tls_pair_size(1705, 1708), 0, old, reserve, needed));
        // A pair that does not grow the data needs only the room next to the old data, not the reserve
        CHECK(web_tls_nvs_room(120, web_tls_pair_size(1705, 1708), 0, old, reserve, needed) && needed == web_tls_nvs_entries_max(3415));
        CHECK(!web_tls_nvs_room(110, web_tls_pair_size(1705, 1708), 0, old, reserve, needed) && needed == web_tls_nvs_entries_max(3415));
        // No old data: the new pair and the reserve
        CHECK(!web_tls_nvs_room(335, web_tls_pair_size(1705, 1708), 0, 0, reserve, needed) && needed == 111 + 225);
        CHECK(web_tls_nvs_room(336, web_tls_pair_size(1705, 1708), 0, 0, reserve, needed));
        // New 256 kB table (7561 available with the stored state): the largest pair fits
        CHECK(web_tls_nvs_room(7561, web_tls_pair_size(8192, 4096), 0, old, reserve, needed));
    }
    // ---- The rule (web_tls_own_*) against a model of the three firmware generations ----
    //
    // A pair is a letter. "ocrt" holds the chain of a pair, "okey" its key or the marker M, "opair" and "opair2" a
    // whole pair. NVS changes one item all or nothing (web_tls_pair.h), so a step happened or not; a chip reset can
    // come after any step. What each firmware does, from its code:
    // G0 = 2102115 (git show 2102115:main/web/web_tls.cpp)
    //   start (web_tls_load_or_create 449-493, own items 466-486): reads mode, then ocrt and okey with read_pem (103-114: a blob without
    //     a NUL at the end is no text, so the marker is no key); a chain that fails the check frees chain and key, a
    //     key without a chain stays in RAM.
    //   store (web_tls_set 500-555): set ocrt, set okey, set mode, commit (532-539); the first error ends it with 400 and
    //     RAM keeps the old pair; "own" without texts stores the pair in RAM again (no unchanged check).
    //   self: set mode 0. Reset to defaults (web_tls_clear_own 584-593): erase ocrt, okey, mode, errors ignored.
    // G1 = 478c2ff to d77f972 (main/web/web_tls.cpp of d77f972)
    //   start (load_own 154-183): opair there -> used, then erase ocrt, okey (errors ignored); else like G0.
    //   store (web_tls_set 610-690, store_pair 215-226): the pair in RAM again -> only mode; else set opair, erase
    //     ocrt, okey (errors ignored), set mode; an error of opair or mode ends it with 400, RAM keeps the old pair.
    //   Reset to defaults (710-721): erase opair, ocrt, okey, mode, errors ignored.
    // G2 = this firmware: web_tls_own_start/store/clear as web_tls.cpp executes them (each plan stops at its first
    //   error; Reset to defaults erases mode also then; a store whose error came after the commit marks RAM as differing
    //   from NVS, so the next store of any pair writes it).
    //
    // The walk starts at an empty NVS and follows every sequence of starts of the three firmwares (an update or a
    // downgrade is a start of another one), stores (of a new pair and of every pair NVS or RAM still holds), self and
    // Reset to defaults, each step succeeding, failing (a write: NVS full; an erase only with erase_faults: a flash
    // error) or followed by a chip reset. Pairs are renamed in the order they appear, so the states are finite.
    // Besides the items a state holds what the requirements say may be in use: the pair of the last store answered with
    // 200 (none after a Reset to defaults that went through) and the pairs of stores and resets since then that failed
    // or were cut off. At each start of G2 its decision must be one of them (R1, R3); a pair that no item holds any more
    // cannot be, then none is allowed. At that start the set becomes the decision (the user sees it from then on).
    // After a Reset to defaults that went through, nothing of an own pair may be left after G2's start (R2).
    // When the firmware before (G0, G1) itself lost the pair that must be in use (G1's start erases a newer ocrt/okey; a
    // store of G0 that fails after its first write overwrites the chain; a G1 store answered with 200 after its RAM
    // and NVS differed), the history is marked "lost": counted and shown, not held against G2.
    // Two histories that end in the same items but allow disjoint decisions are a conflict: no rule that sees only the
    // items can meet both. The test lists every conflict with a shortest history of each side.

    constexpr int8_t ABSENT = -1;
    constexpr int8_t MARKER = -2; // okey only
    constexpr int8_t NONE = -1;   // no pair
    constexpr int8_t GONE = -3;   // committed: a pair no item and no RAM holds any more
    constexpr int IDS = 6;
    constexpr int NONE_BIT = 7;

    struct Items
    {
        int8_t ocrt = ABSENT, okey = ABSENT, opair = ABSENT, opair2 = ABSENT, mode = ABSENT;
    };

    struct State
    {
        Items it;
        int8_t gen = -1;         // 0, 1, 2; -1: restarting (the next action is a start)
        int8_t ram_a = ABSENT;   // G0/G1: the pair of s_own_cert; G2: the pair in RAM
        int8_t ram_b = ABSENT;   // G0/G1: the pair of s_own_key
        int8_t differs = 0;      // G2: NVS holds another pair than RAM
        int8_t committed = NONE; // the pair that must be in use (see above)
        uint8_t pending = 0;     // pairs that may be in use instead (bit NONE_BIT: none)
        int8_t lost = 0;
        int8_t cleared = 0; // a Reset to defaults went through and no store was tried since
        int8_t faults = 0;  // steps that failed or were cut off in the history (counted up to 2)
    };

    // What web_tls.cpp reads: which items exist, the class of okey, which halves are equal (new_pair: a store's pair)
    web_tls_own_items_t presence(const Items &it, int8_t new_pair = NONE)
    {
        web_tls_own_items_t p;
        p.ocrt = it.ocrt >= 0;
        p.okey = it.okey == MARKER ? web_tls_okey_t::marker : it.okey >= 0 ? web_tls_okey_t::key : web_tls_okey_t::absent;
        p.opair = it.opair >= 0;
        p.opair2 = it.opair2 >= 0;
        p.mode = it.mode >= 0;
        auto in = [&](int8_t half)
        {
            return static_cast<uint8_t>((half >= 0 && half == it.opair ? WEB_TLS_OWN_IN_OPAIR : 0) | (half >= 0 && half == it.opair2 ? WEB_TLS_OWN_IN_OPAIR2 : 0));
        };
        p.ocrt_in = in(it.ocrt);
        p.okey_in = in(it.okey);
        p.new_in = static_cast<uint8_t>((new_pair >= 0 && it.ocrt == new_pair ? 1 : 0) | (new_pair >= 0 && it.okey == new_pair ? 2 : 0));
        return p;
    }

    // The pair G2 uses: the source of the rule, and former only when chain and key are of the same pair (the check)
    int8_t decision(const Items &it)
    {
        switch (web_tls_own_decide(presence(it)))
        {
        case web_tls_own_source_t::former:
            return it.ocrt >= 0 && it.ocrt == it.okey ? it.ocrt : NONE;
        case web_tls_own_source_t::opair:
            return it.opair;
        case web_tls_own_source_t::opair2:
            return it.opair2;
        default:
            return NONE;
        }
    }

    bool recoverable(const Items &it, int8_t id)
    {
        return id >= 0 && ((it.ocrt == id && it.okey == id) || it.opair == id || it.opair2 == id);
    }

    bool key_material(const Items &it)
    {
        return it.ocrt >= 0 || it.okey >= 0 || it.opair >= 0 || it.opair2 >= 0;
    }

    uint8_t bit_of(int8_t id)
    {
        return static_cast<uint8_t>(1u << (id >= 0 ? id : NONE_BIT));
    }

    // The decisions allowed at a start of G2 in this history
    uint8_t allowed(const State &s)
    {
        uint8_t mask = s.committed >= 0 && recoverable(s.it, s.committed) ? bit_of(s.committed) : bit_of(NONE);
        if (s.pending & bit_of(NONE))
            mask |= bit_of(NONE);
        for (int8_t id = 0; id < IDS; ++id)
            if ((s.pending & bit_of(id)) && recoverable(s.it, id))
                mask |= bit_of(id);
        return mask;
    }

    // Renames the pairs in the order they appear; back[new] = the old number
    State canonical(State s, int8_t *back = nullptr)
    {
        if (s.gen < 0)
        {
            s.ram_a = s.ram_b = ABSENT;
            s.differs = 0;
        }
        int8_t map[IDS];
        for (int8_t &m : map)
            m = -1;
        int8_t next = 0;
        auto rename = [&](int8_t &v)
        {
            if (v < 0)
                return;
            if (map[v] < 0)
            {
                if (back != nullptr)
                    back[next] = v;
                map[v] = next++;
            }
            v = map[v];
        };
        rename(s.it.ocrt);
        rename(s.it.okey);
        rename(s.it.opair);
        rename(s.it.opair2);
        rename(s.ram_a);
        rename(s.ram_b);
        if (s.committed >= 0)
            s.committed = map[s.committed] >= 0 ? map[s.committed] : GONE;
        uint8_t pending = s.pending & bit_of(NONE);
        for (int8_t id = 0; id < IDS; ++id)
            if ((s.pending & bit_of(id)) && map[id] >= 0)
                pending |= bit_of(map[id]);
        s.pending = pending;
        return s;
    }

    uint64_t key_of(const State &s)
    {
        uint64_t k = 0;
        auto put = [&](int v, int bits) { k = (k << bits) | static_cast<uint64_t>(v & ((1 << bits) - 1)); };
        put(s.it.ocrt + 4, 4);
        put(s.it.okey + 4, 4);
        put(s.it.opair + 4, 4);
        put(s.it.opair2 + 4, 4);
        put(s.it.mode + 1, 2);
        put(s.gen + 1, 2);
        put(s.ram_a + 4, 4);
        put(s.ram_b + 4, 4);
        put(s.differs, 1);
        put(s.committed + 4, 4);
        put(s.pending, 8);
        put(s.lost, 1);
        put(s.cleared, 1);
        put(s.faults, 2);
        return k;
    }

    constexpr int ITEM_SHIFT = 27; // the item fields are the top 18 bits of key_of

    uint32_t items_key(const Items &it)
    {
        State s;
        s.it = it;
        return static_cast<uint32_t>(key_of(canonical(s)) >> ITEM_SHIFT);
    }

    Items items_from_key(uint32_t key)
    {
        const uint64_t k = static_cast<uint64_t>(key) << ITEM_SHIFT;
        auto get = [&](int shift, int bits) { return static_cast<int8_t>((k >> (ITEM_SHIFT + shift)) & ((1u << bits) - 1)); };
        Items it;
        it.mode = static_cast<int8_t>(get(0, 2) - 1);
        it.opair2 = static_cast<int8_t>(get(2, 4) - 4);
        it.opair = static_cast<int8_t>(get(6, 4) - 4);
        it.okey = static_cast<int8_t>(get(10, 4) - 4);
        it.ocrt = static_cast<int8_t>(get(14, 4) - 4);
        return it;
    }

    // Letters for the pairs; labels maps a number to the letter of a history (nullptr: A, B, ...)
    std::string pair_text(int8_t id, const char *labels = nullptr)
    {
        if (id == ABSENT)
            return "-";
        if (id == MARKER)
            return "M";
        return std::string(1, labels != nullptr ? labels[id] : static_cast<char>('A' + id));
    }

    std::string items_text(const Items &it, const char *labels = nullptr)
    {
        return "ocrt=" + pair_text(it.ocrt, labels) + " okey=" + pair_text(it.okey, labels) + " opair=" + pair_text(it.opair, labels) +
               " opair2=" + pair_text(it.opair2, labels) + " mode=" + (it.mode < 0 ? std::string("-") : std::to_string(it.mode));
    }

    // A failed check of the walk with the items before and after (the first WEB_TLS_PAIR_SHOW, default 20)
    int g_shown = 0;
    const int g_show_max = getenv("WEB_TLS_PAIR_SHOW") != nullptr ? atoi(getenv("WEB_TLS_PAIR_SHOW")) : 20;
    void check_items(bool ok, const char *what, const Items &before, const Items &after)
    {
        CHECK(ok);
        if (!ok && g_shown++ < g_show_max)
            fprintf(stderr, "    %s: %s -> %s\n", what, items_text(before).c_str(), items_text(after).c_str());
    }

    enum class Op : uint8_t
    {
        set_ocrt,
        set_okey,
        set_opair,
        set_opair2,
        set_marker,
        copy_opair,
        set_mode,
        erase_ocrt,
        erase_okey,
        erase_opair,
        erase_opair2,
        erase_mode
    };

    struct Step
    {
        Op op;
        int8_t value;
        bool abort_on_fail;
    };

    bool is_erase(Op op)
    {
        return op >= Op::erase_ocrt;
    }

    void apply(Items &it, const Step &st)
    {
        switch (st.op)
        {
        case Op::set_ocrt: it.ocrt = st.value; break;
        case Op::set_okey: it.okey = st.value; break;
        case Op::set_opair: it.opair = st.value; break;
        case Op::set_opair2: it.opair2 = st.value; break;
        case Op::set_marker: it.okey = MARKER; break;
        case Op::copy_opair: it.opair2 = it.opair; break;
        case Op::set_mode: it.mode = st.value; break;
        case Op::erase_ocrt: it.ocrt = ABSENT; break;
        case Op::erase_okey: it.okey = ABSENT; break;
        case Op::erase_opair: it.opair = ABSENT; break;
        case Op::erase_opair2: it.opair2 = ABSENT; break;
        case Op::erase_mode: it.mode = ABSENT; break;
        }
    }

    Step step_of(web_tls_own_step_t step, int8_t new_pair)
    {
        switch (step)
        {
        case web_tls_own_step_t::erase_ocrt: return {Op::erase_ocrt, 0, true};
        case web_tls_own_step_t::erase_okey: return {Op::erase_okey, 0, true};
        case web_tls_own_step_t::erase_opair: return {Op::erase_opair, 0, true};
        case web_tls_own_step_t::erase_opair2: return {Op::erase_opair2, 0, true};
        case web_tls_own_step_t::write_marker: return {Op::set_marker, 0, true};
        case web_tls_own_step_t::copy_opair: return {Op::copy_opair, 0, true};
        case web_tls_own_step_t::write_new:
        default: return {Op::set_opair2, new_pair, true};
        }
    }

    std::vector<Step> plan_steps(const web_tls_own_plan_t &plan, int8_t new_pair)
    {
        std::vector<Step> steps;
        for (uint8_t i = 0; i < plan.count; ++i)
            steps.push_back(step_of(plan.steps[i], new_pair));
        return steps;
    }

    struct Outcome
    {
        Items it;
        enum Kind : uint8_t
        {
            done,
            failed,
            cut
        } kind;
        int index;  // failed: the step that failed; cut: the last step done
        bool fault; // an erase failed and was ignored
    };

    // Every way the steps can go: each step done (then a chip reset may follow) or failing (nothing changes; the
    // firmware stops with an error, or ignores it and goes on)
    void run(const Items &it, const std::vector<Step> &steps, size_t i, bool erase_faults, bool fault, std::vector<Outcome> &out)
    {
        if (i == steps.size())
        {
            out.push_back({it, Outcome::done, static_cast<int>(i), fault});
            return;
        }
        const Step &st = steps[i];
        Items ok = it;
        apply(ok, st);
        out.push_back({ok, Outcome::cut, static_cast<int>(i), fault});
        run(ok, steps, i + 1, erase_faults, fault, out);
        if (!is_erase(st.op) || erase_faults)
        {
            if (st.abort_on_fail)
                out.push_back({it, Outcome::failed, static_cast<int>(i), fault || is_erase(st.op)});
            else
                run(it, steps, i + 1, erase_faults, true, out);
        }
    }

    // G0 and G1 read the former pair as load_or_create of 2102115 does
    void read_former(State &s)
    {
        s.ram_a = s.it.ocrt >= 0 ? s.it.ocrt : ABSENT;
        s.ram_b = s.it.okey >= 0 ? s.it.okey : ABSENT;
        if (s.ram_a >= 0 && s.ram_a != s.ram_b)
            s.ram_a = s.ram_b = ABSENT;
    }

    const char *gen_name(int8_t gen)
    {
        return gen == 0 ? "2102115" : gen == 1 ? "478c2ff" : "new";
    }

    struct Seen
    {
        int8_t decision = NONE;
        std::map<uint8_t, uint32_t> masks; // allowed set -> the state before the start (BFS: a shortest history)
        uint32_t r2_failed = UINT32_MAX;   // a history where R2 was not met at this start
        uint8_t at_most_one = 0xFF;        // the decisions all histories with at most one fault allow
        std::map<uint8_t, uint32_t> masks_at_most_one;
        bool any_at_most_one = false;
        uint32_t r2_failed_at_most_one = UINT32_MAX;
    };

    struct Walk
    {
        bool erase_faults = false;
        std::unordered_map<uint64_t, uint32_t> index;
        std::vector<State> states;
        std::vector<uint32_t> parent;
        std::vector<std::array<int8_t, IDS>> back; // pair numbers of a state -> those of its parent (-1: new)
        std::vector<std::string> how;
        std::vector<int8_t> how_pair; // the pair of the action, numbered as in the parent
        std::map<uint32_t, Seen> clean, lost; // by the items at a start of G2
        size_t starts = 0, r2_checks = 0, store_checks = 0;

        void add(State raw, uint32_t from, const std::string &text, int8_t pair = NONE)
        {
            if (text.find("failed") != std::string::npos || text.find("chip reset") != std::string::npos || text.find("cut after") != std::string::npos)
                raw.faults = static_cast<int8_t>(raw.faults < 2 ? raw.faults + 1 : 2);
            std::array<int8_t, IDS> b;
            b.fill(-1);
            const State s = canonical(raw, b.data());
            // A new pair has no number in the parent: it has none in b either
            if (from != UINT32_MAX)
                for (int8_t &v : b)
                    if (v >= 0 && !number_used(states[from], v))
                        v = -1;
            const uint64_t k = key_of(s);
            if (index.find(k) != index.end())
                return;
            index.emplace(k, static_cast<uint32_t>(states.size()));
            states.push_back(s);
            parent.push_back(from);
            back.push_back(b);
            how.push_back(text);
            how_pair.push_back(pair);
        }

        static bool number_used(const State &s, int8_t v)
        {
            return s.it.ocrt == v || s.it.okey == v || s.it.opair == v || s.it.opair2 == v || s.ram_a == v || s.ram_b == v;
        }

        // After a step of an older firmware: it lost the pair that must be in use itself
        static void mark_lost(State &s)
        {
            if (s.committed >= 0 && !recoverable(s.it, s.committed))
                s.lost = 1;
        }

        void start(const State &s, uint32_t from, int8_t gen)
        {
            State base = s;
            base.gen = gen;
            base.ram_a = base.ram_b = ABSENT;
            base.differs = 0;
            if (gen == 0 || (gen == 1 && base.it.opair < 0))
            {
                read_former(base);
                add(base, from, std::string("start ") + gen_name(gen));
                return;
            }
            if (gen == 1)
            {
                std::vector<Outcome> out;
                run(base.it, {{Op::erase_ocrt, 0, false}, {Op::erase_okey, 0, false}}, 0, erase_faults, false, out);
                for (const Outcome &o : out)
                {
                    State n = base;
                    n.it = o.it;
                    mark_lost(n);
                    if (o.kind == Outcome::cut)
                        n.gen = -1;
                    else
                        n.ram_a = n.ram_b = base.it.opair;
                    add(n, from, std::string("start 478c2ff") + (o.kind == Outcome::cut ? ", cut after its erase " + std::to_string(o.index) : "") + (o.fault ? " (an erase failed)" : ""));
                }
                return;
            }
            // G2: the decision against what the history allows
            ++starts;
            const int8_t d = decision(base.it);
            Seen &seen = (base.lost ? lost : clean)[items_key(base.it)];
            seen.decision = d;
            seen.masks.emplace(allowed(base), from);
            if (base.faults <= 1)
            {
                seen.at_most_one &= allowed(base);
                seen.masks_at_most_one.emplace(allowed(base), from);
                seen.any_at_most_one = true;
            }
            const web_tls_own_plan_t plan = web_tls_own_start(presence(base.it));
            std::vector<Outcome> out;
            run(base.it, plan_steps(plan, NONE), 0, erase_faults, false, out);
            for (const Outcome &o : out)
            {
                check_items(decision(o.it) == d, "start: decision kept", base.it, o.it);
                if (o.kind == Outcome::done && base.cleared && !base.lost)
                {
                    ++r2_checks;
                    if (key_material(o.it) && seen.r2_failed == UINT32_MAX)
                        seen.r2_failed = from;
                    if (key_material(o.it) && base.faults <= 1 && seen.r2_failed_at_most_one == UINT32_MAX)
                        seen.r2_failed_at_most_one = from;
                }
                State n = base;
                n.it = o.it;
                n.committed = d;
                n.pending = 0;
                n.lost = 0;
                if (o.kind == Outcome::cut)
                    n.gen = -1;
                else
                    n.ram_a = d;
                add(n, from, o.kind == Outcome::cut ? "start new, cut after step " + std::to_string(o.index)
                                                    : o.kind == Outcome::failed ? "start new, step " + std::to_string(o.index) + " failed" : "start new");
            }
        }

        void store(const State &s, uint32_t from, int8_t pair)
        {
            std::vector<Step> steps;
            int commit = 0;
            const int8_t before = decision(s.it);
            const bool same = s.gen == 0 ? false : s.gen == 1 ? s.ram_a == pair && s.ram_b == pair : s.ram_a == pair && !s.differs;
            if (same)
                steps = {{Op::set_mode, 1, true}};
            else if (s.gen == 0)
                steps = {{Op::set_ocrt, pair, true}, {Op::set_okey, pair, true}, {Op::set_mode, 1, true}};
            else if (s.gen == 1)
                steps = {{Op::set_opair, pair, true}, {Op::erase_ocrt, 0, false}, {Op::erase_okey, 0, false}, {Op::set_mode, 1, true}};
            else
            {
                const web_tls_own_plan_t plan = web_tls_own_store(presence(s.it, pair));
                steps = plan_steps(plan, pair);
                commit = plan.commit;
                CHECK(plan.commit < plan.count);
                steps.push_back({Op::set_mode, 1, true});
            }
            std::vector<Outcome> out;
            run(s.it, steps, 0, erase_faults, false, out);
            for (const Outcome &o : out)
            {
                State n = s;
                n.it = o.it;
                n.cleared = 0;
                std::string text = std::string("store %s with ") + gen_name(s.gen) + (same ? " (unchanged)" : "");
                if (o.kind == Outcome::done)
                {
                    n.committed = pair;
                    n.pending = 0;
                    n.lost = 0;
                    if (s.gen < 2)
                        n.ram_a = n.ram_b = pair;
                    else
                    {
                        n.ram_a = pair;
                        n.differs = 0;
                    }
                    text += ": 200";
                }
                else
                {
                    n.pending |= bit_of(pair);
                    if (o.kind == Outcome::cut)
                    {
                        n.gen = -1;
                        text += ": chip reset after step " + std::to_string(o.index);
                    }
                    else
                    {
                        text += ": step " + std::to_string(o.index) + " failed";
                        if (s.gen == 2 && !same && o.index > commit)
                            n.differs = 1;
                    }
                }
                if (o.fault)
                    text += " (an erase failed)";
                if (s.gen < 2)
                    mark_lost(n);
                else if (!same)
                {
                    // R3 for this firmware: the old pair up to the commit, the new one from it on
                    ++store_checks;
                    const int last = o.kind == Outcome::failed ? o.index - 1 : o.index;
                    const int8_t after = decision(o.it);
                    if (o.kind == Outcome::done || last >= commit)
                        check_items(after == pair, "store: the new pair from the commit on", s.it, o.it);
                    else
                        check_items(after == before, "store: the old pair before the commit", s.it, o.it);
                }
                add(n, from, text, pair);
            }
        }

        void self(const State &s, uint32_t from)
        {
            std::vector<Outcome> out;
            run(s.it, {{Op::set_mode, 0, true}}, 0, erase_faults, false, out);
            for (const Outcome &o : out)
            {
                State n = s;
                n.it = o.it;
                if (o.kind == Outcome::cut)
                    n.gen = -1;
                add(n, from, std::string("self with ") + gen_name(s.gen) + (o.kind == Outcome::failed ? ": failed" : o.kind == Outcome::cut ? ": chip reset" : ""));
            }
        }

        void reset(const State &s, uint32_t from)
        {
            std::vector<Outcome> out;
            const int8_t before = decision(s.it);
            if (s.gen < 2)
            {
                std::vector<Step> steps = {{Op::erase_ocrt, 0, false}, {Op::erase_okey, 0, false}, {Op::erase_mode, 0, false}};
                if (s.gen == 1)
                    steps.insert(steps.begin(), {Op::erase_opair, 0, false});
                run(s.it, steps, 0, erase_faults, false, out);
            }
            else
            {
                // The plan stops at its first error; mode is erased after it either way
                std::vector<Outcome> plan_out;
                run(s.it, plan_steps(web_tls_own_clear(presence(s.it)), NONE), 0, erase_faults, false, plan_out);
                for (const Outcome &p : plan_out)
                {
                    if (p.kind == Outcome::cut)
                    {
                        out.push_back(p);
                        continue;
                    }
                    std::vector<Outcome> mode_out;
                    run(p.it, {{Op::erase_mode, 0, false}}, 0, erase_faults, p.kind == Outcome::failed || p.fault, mode_out);
                    for (Outcome m : mode_out)
                    {
                        if (m.kind == Outcome::cut)
                            m.index = -1; // after the plan
                        out.push_back(m);
                    }
                }
            }
            for (const Outcome &o : out)
            {
                State n = s;
                n.it = o.it;
                n.gen = -1;
                std::string text = std::string("reset with ") + gen_name(s.gen);
                if (o.kind == Outcome::done && !o.fault)
                {
                    n.committed = NONE;
                    n.pending = 0;
                    n.lost = 0;
                    n.cleared = 1;
                }
                else
                {
                    n.pending |= bit_of(NONE);
                    text += o.kind == Outcome::cut ? ": chip reset after step " + std::to_string(o.index) : ": an erase failed";
                }
                // A reset of an older firmware loses nothing it must keep: none is allowed after it
                if (s.gen == 2)
                {
                    const int8_t after = decision(o.it);
                    check_items(after == before || after == NONE, "clear: the old pair or none", s.it, o.it);
                    if (o.kind == Outcome::done && !o.fault)
                        check_items(!key_material(o.it) && o.it.okey == ABSENT && o.it.mode == ABSENT, "clear: nothing left", s.it, o.it);
                }
                add(n, from, text);
            }
        }

        void expand(uint32_t i)
        {
            const State s = states[i];
            for (int8_t gen = 0; gen < 3; ++gen)
                start(s, i, gen);
            if (s.gen < 0)
                return;
            // Store a new pair, or one that NVS or RAM still holds
            bool fresh = false;
            for (int8_t id = 0; id < IDS; ++id)
            {
                if (number_used(s, id))
                    store(s, i, id);
                else if (!fresh)
                {
                    fresh = true;
                    store(s, i, id);
                }
            }
            self(s, i);
            reset(s, i);
        }

        void walk()
        {
            add(State{}, UINT32_MAX, "empty NVS");
            for (uint32_t i = 0; i < states.size(); ++i)
                expand(i);
        }

        // The history of a state with the same letter for a pair from start to end
        std::string history(uint32_t i, const char *extra = nullptr) const
        {
            std::vector<uint32_t> path;
            for (uint32_t j = i; j != UINT32_MAX; j = parent[j])
                path.push_back(j);
            char labels[IDS] = {};
            char next = 'A';
            std::string text;
            for (size_t k = path.size(); k-- > 0;)
            {
                const uint32_t j = path[k];
                char child[IDS] = {};
                for (int8_t v = 0; v < IDS; ++v)
                    child[v] = back[j][v] >= 0 ? labels[back[j][v]] : '?';
                // A pair new in this step gets the next letter (one per step at most)
                std::string line = how[j];
                const size_t at = line.find("%s");
                char fresh_label = 0;
                for (int8_t v = 0; v < IDS; ++v)
                    if (child[v] == '?')
                        child[v] = fresh_label != 0 ? fresh_label : (fresh_label = next++);
                if (at != std::string::npos)
                {
                    const int8_t pair = how_pair[j];
                    const char name = pair >= 0 && pair < IDS && labels[pair] != 0 && number_used_parent(j, pair) ? labels[pair] : (fresh_label != 0 ? fresh_label : '?');
                    line.replace(at, 2, std::string(1, name));
                }
                for (int8_t v = 0; v < IDS; ++v)
                    labels[v] = child[v];
                text += "      " + line + " -> " + items_text(states[j].it, labels) + "\n";
            }
            if (extra != nullptr)
                text += std::string("      ") + extra + "\n";
            return text;
        }

        bool number_used_parent(uint32_t j, int8_t pair) const
        {
            return parent[j] != UINT32_MAX && number_used(states[parent[j]], pair);
        }
    };

    std::string mask_text(uint8_t mask)
    {
        std::string t = "{";
        for (int8_t id = 0; id < IDS; ++id)
            if (mask & bit_of(id))
                t += std::string(t.size() > 1 ? "," : "") + static_cast<char>('A' + id);
        if (mask & bit_of(NONE))
            t += std::string(t.size() > 1 ? "," : "") + "none";
        return t + "}";
    }

    // The conflicts: items -> the decision of the rule
    struct Report
    {
        size_t conflicts = 0, violations = 0, r2_failed = 0;   // over all histories
        std::vector<std::string> named;                       // with at most one fault: conflicts and R2 not met
        size_t violations_at_most_one = 0;
    };

    // Counts and prints: every history of a conflict or of R2 not met with at most one fault (they are the ones the
    // commit message names); WEB_TLS_PAIR_VERBOSE: also those with more faults and the lost ones
    Report report(const Walk &w, const char *title)
    {
        Report r;
        const bool verbose = getenv("WEB_TLS_PAIR_VERBOSE") != nullptr;
        for (const auto &[key, seen] : w.clean)
        {
            uint8_t all = 0xFF;
            for (const auto &[mask, from] : seen.masks)
                all &= mask;
            const std::string items = items_text(items_from_key(key));
            const std::string decision_text = seen.decision >= 0 ? pair_text(seen.decision) : "none";
            if (seen.r2_failed != UINT32_MAX)
                ++r.r2_failed;
            if (all == 0)
                ++r.conflicts;
            else if (!(all & bit_of(seen.decision)))
                ++r.violations;
            const bool one_conflict = seen.any_at_most_one && seen.at_most_one == 0;
            const bool one_violation = seen.any_at_most_one && seen.at_most_one != 0 && !(seen.at_most_one & bit_of(seen.decision));
            if (one_violation)
                ++r.violations_at_most_one;
            if (one_conflict)
                r.named.push_back("conflict at " + items + ", decision " + decision_text);
            if (seen.r2_failed_at_most_one != UINT32_MAX)
                r.named.push_back("R2 not met at " + items);
            const bool show = one_conflict || one_violation || seen.r2_failed_at_most_one != UINT32_MAX || (verbose && (all == 0 || !(all & bit_of(seen.decision))));
            if (!show)
                continue;
            printf("    %s at %s: decision %s; allowed by all histories %s, by those with at most one fault %s\n",
                   one_violation ? "VIOLATION" : all == 0 ? "conflict" : !(all & bit_of(seen.decision)) ? "not met" : "R2 not met", items.c_str(),
                   decision_text.c_str(), mask_text(all).c_str(), mask_text(seen.at_most_one).c_str());
            for (const auto &[mask, from] : verbose ? seen.masks : seen.masks_at_most_one)
                    printf("      allowed %s%s, after (%d faults):\n%s", mask_text(mask).c_str(), (mask & bit_of(seen.decision)) ? "" : " (not met)",
                           w.states[from].faults, w.history(from, "start new").c_str());
            if (seen.r2_failed_at_most_one != UINT32_MAX)
                printf("      R2 not met after:\n%s", w.history(seen.r2_failed_at_most_one, "start new").c_str());
        }
        size_t lost_unmet = 0;
        for (const auto &[key, seen] : w.lost)
            for (const auto &[mask, from] : seen.masks)
                if (!(mask & bit_of(seen.decision)))
                {
                    ++lost_unmet;
                    if (verbose)
                        printf("    lost by the firmware before, allowed %s, decision %s, after:\n%s", mask_text(mask).c_str(), pair_text(seen.decision).c_str(),
                               w.history(from, "start new").c_str());
                }
        printf("  %s: %zu states, %zu item states at a start of the new firmware, %zu checks of R2, %zu stores of the new firmware checked\n", title,
               w.states.size(), w.clean.size(), w.r2_checks, w.store_checks);
        printf("  %s: all histories: %zu item states with a conflict, %zu where the decision is not what all allow, %zu with R2 not met\n", title,
               r.conflicts, r.violations, r.r2_failed);
        printf("  %s: histories with at most one fault: %zu violations; named: %zu\n", title, r.violations_at_most_one, r.named.size());
        for (const std::string &line : r.named)
            printf("    %s\n", line.c_str());
        printf("  %s: the firmware before lost the pair itself at %zu item states (%zu histories with another decision)\n", title, w.lost.size(), lost_unmet);
        CHECK(r.violations_at_most_one == 0);
        return r;
    }

    // Every combination of items: the plans keep the decision at every step (start), go from the old to the new pair
    // at the commit (store), and leave nothing (clear)
    void test_rule_all_items()
    {
        const int8_t ids[] = {ABSENT, 0, 1, 2, 3};
        const int8_t okeys[] = {ABSENT, MARKER, 0, 1, 2, 3};
        const int8_t modes[] = {ABSENT, 0, 1};
        size_t combinations = 0;
        for (int8_t c : ids)
            for (int8_t k : okeys)
                for (int8_t p : ids)
                    for (int8_t p2 : ids)
                        for (int8_t m : modes)
                        {
                            ++combinations;
                            const Items it{c, k, p, p2, m};
                            const int8_t d = decision(it);
                            // start: every prefix keeps the decision; afterwards nothing is left that does not count
                            const web_tls_own_plan_t start = web_tls_own_start(presence(it));
                            Items after = it;
                            for (uint8_t i = 0; i < start.count; ++i)
                            {
                                apply(after, step_of(start.steps[i], NONE));
                                check_items(decision(after) == d, "all items, start", it, after);
                            }
                            CHECK(web_tls_own_start(presence(after)).count == 0); // a second start changes nothing
                            if (start.source == web_tls_own_source_t::none)
                                CHECK(after.okey < 0 && after.opair < 0 && after.opair2 < 0);
                            else if (start.source != web_tls_own_source_t::former)
                                CHECK(after.opair2 == d && after.okey == MARKER && after.opair < 0 && after.ocrt < 0);
                            else
                                CHECK(after.opair < 0 && after.opair2 < 0);
                            // store of a new pair (9) and of each pair there
                            for (int8_t x : {int8_t(9), int8_t(0), int8_t(1)})
                            {
                                const web_tls_own_plan_t store = web_tls_own_store(presence(it, x));
                                CHECK(store.commit < store.count);
                                Items s = it;
                                for (uint8_t i = 0; i < store.count; ++i)
                                {
                                    apply(s, step_of(store.steps[i], x));
                                    check_items(decision(s) == (i >= store.commit ? x : d), "all items, store", it, s);
                                }
                                CHECK(s.opair2 == x && s.okey == MARKER && s.opair < 0 && s.ocrt < 0);
                            }
                            // clear: every prefix leaves the old pair or none, the end nothing
                            const web_tls_own_plan_t clear = web_tls_own_clear(presence(it));
                            Items cl = it;
                            for (uint8_t i = 0; i < clear.count; ++i)
                            {
                                apply(cl, step_of(clear.steps[i], NONE));
                                check_items(decision(cl) == d || decision(cl) == NONE, "all items, clear", it, cl);
                            }
                            CHECK(!key_material(cl) && cl.okey == ABSENT);
                        }
        CHECK(combinations == 5 * 6 * 5 * 5 * 3);
    }

    void test_rule_walk()
    {
        // In every history the decision is one that all histories ending in the same items allow, when there is one.
        // The named ones (at most one fault, no common decision or R2 not met) all come from one sequence: a pair
        // stored by 478c2ff..d77f972 in opair, then Reset to defaults on 2102115 (it does not know opair) and mode
        // written again there (self) or that reset cut off before its mode; the items are those of a board of
        // 478c2ff..d77f972 (R4). A change of the rule that changes these numbers must be looked at again.
        Walk plain;
        plain.walk();
        const Report r = report(plain, "without erase errors");
        CHECK(r.violations == 0);
        CHECK(r.named.size() == 15);
        for (const std::string &line : r.named)
            CHECK(line.find("opair=-") == std::string::npos || line.find("okey=M") != std::string::npos);
        Walk faults;
        faults.erase_faults = true;
        faults.walk();
        const Report f = report(faults, "with erase errors");
        CHECK(f.violations == 0);
        CHECK(f.named.size() == 36);
    }
} // namespace

void run_tests()
{
    test_pack_split();
    test_split_refused();
    test_entries();
    test_room();
    test_rule_all_items();
    test_rule_walk();
}

// Any blob: a split that succeeds gives two non-empty texts that pack to the same bytes
void fuzz_one(const uint8_t *data, size_t size)
{
    const char *cert = nullptr;
    const char *key = nullptr;
    if (!web_tls_pair_split(data, size, cert, key))
        return;
    CHECK(cert[0] != '\0' && key[0] != '\0');
    std::vector<uint8_t> again(size);
    CHECK(web_tls_pair_pack(cert, key, again.data(), again.size()) == size);
    CHECK(memcmp(again.data(), data, size) == 0);
}
