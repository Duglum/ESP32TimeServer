// ESP32 Time Server host tests: building cJSON trees when memory is short (main/core/json_tree.cpp; the test of
// ESP32-WebTherm's output_test of 76618ac, with json_object_add() and the shapes of /api/diag)
#define TEST_NAME "json_tree_test"
#include "check.h"

#include "core/json_tree.h"
#include "cJSON.h"

#include <cstdlib>
#include <cstring>
#include <set>

namespace
{
    long g_live = 0;        // blocks allocated through the hooks and not freed
    long g_allocations = 0; // allocations asked for since the last reset
    long g_fail_at = -1;    // this allocation (counted from 0) fails
    std::set<void *> g_blocks;

    void *counted_malloc(size_t size)
    {
        if (g_allocations++ == g_fail_at)
            return nullptr;
        void *block = malloc(size);
        if (block == nullptr)
            return nullptr;
        ++g_live;
        g_blocks.insert(block);
        return block;
    }

    void counted_free(void *block)
    {
        if (block != nullptr)
        {
            --g_live;
            g_blocks.erase(block);
        }
        free(block);
    }

    bool g_raw = false; // cJSON's own calls, for the control

    void add(cJSON *array, cJSON *item)
    {
        if (g_raw)
            cJSON_AddItemToArray(array, item);
        else
            json_array_add(array, item);
    }

    void add_named(cJSON *object, const char *name, cJSON *item)
    {
        if (g_raw)
            cJSON_AddItemToObject(object, name, item);
        else
            json_object_add(object, name, item);
    }

    // A tree as the firmware builds one: lists of texts (recovery codes, addresses), rows of objects (RADIUS servers
    // with their secret, tasks), and the number arrays of /api/diag added by name
    void build(cJSON *root, int count)
    {
        static const int bins[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        cJSON *list = cJSON_AddArrayToObject(root, "recovery_codes");
        for (int i = 0; i < count; ++i)
            add(list, cJSON_CreateString("abcde-fghij"));
        cJSON *rows = cJSON_AddArrayToObject(root, "rows");
        for (int i = 0; i < count; ++i)
        {
            cJSON *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "host", "radius.example");
            cJSON_AddStringToObject(row, "secret", "a shared secret");
            add_named(row, "n", cJSON_CreateIntArray(bins, 8));
            add(rows, row);
        }
        add_named(root, "isr_bins", cJSON_CreateIntArray(bins, 6));
        add_named(root, "tx_delay_min_ns", cJSON_CreateIntArray(bins, 2));
    }

    void reset_counts()
    {
        g_allocations = 0;
        g_fail_at = -1;
    }
} // namespace

void run_tests()
{
    cJSON_Hooks hooks{};
    hooks.malloc_fn = counted_malloc;
    hooks.free_fn = counted_free;
    cJSON_InitHooks(&hooks);

    // What cJSON does itself: the item stays with the caller
    cJSON *kept = cJSON_CreateString("a recovery code");
    CHECK(g_live == 2); // the node and its text
    CHECK(!cJSON_AddItemToArray(nullptr, kept));
    CHECK(!cJSON_AddItemToObject(nullptr, "name", kept));
    CHECK(g_live == 2);
    cJSON_Delete(kept);
    CHECK(g_live == 0);

    // json_array_add(): not added -> deleted, with everything it holds
    CHECK(!json_array_add(nullptr, cJSON_CreateString("a recovery code")));
    CHECK(g_live == 0);
    cJSON *object = cJSON_CreateObject();
    cJSON_AddStringToObject(object, "host", "radius.example");
    cJSON_AddStringToObject(object, "secret", "a shared secret");
    CHECK(g_live == 7); // the object; per member its node, text and key
    CHECK(!json_array_add(nullptr, object));
    CHECK(g_live == 0);
    CHECK(!json_array_add(nullptr, nullptr));
    cJSON *array = cJSON_CreateArray();
    CHECK(!json_array_add(array, nullptr));
    CHECK(cJSON_GetArraySize(array) == 0);
    CHECK(json_array_add(array, cJSON_CreateString("one")));
    CHECK(json_array_add(array, cJSON_CreateNumber(2)));
    CHECK(cJSON_GetArraySize(array) == 2);
    CHECK(cJSON_IsString(cJSON_GetArrayItem(array, 0)) && strcmp(cJSON_GetArrayItem(array, 0)->valuestring, "one") == 0);
    cJSON_Delete(array);
    CHECK(g_live == 0);

    // json_object_add(): a null object, a null item, the copy of the name failing -> deleted; added -> found by name
    static const int bins[4] = {10, 20, 30, 40};
    CHECK(!json_object_add(nullptr, "isr_bins", cJSON_CreateIntArray(bins, 4)));
    CHECK(g_live == 0);
    object = cJSON_CreateObject();
    CHECK(!json_object_add(object, "isr_bins", nullptr));
    reset_counts();
    cJSON *numbers = cJSON_CreateIntArray(bins, 4); // the array and its 4 numbers
    CHECK(g_allocations == 5);
    g_fail_at = g_allocations; // the next one: the copy of the name
    CHECK(!json_object_add(object, "isr_bins", numbers));
    CHECK(g_live == 1); // only the object
    CHECK(cJSON_GetObjectItem(object, "isr_bins") == nullptr);
    g_fail_at = -1;
    CHECK(json_object_add(object, "isr_bins", cJSON_CreateIntArray(bins, 4)));
    const cJSON *found = cJSON_GetObjectItem(object, "isr_bins");
    CHECK(cJSON_IsArray(found) && cJSON_GetArraySize(found) == 4 && cJSON_GetArrayItem(found, 3)->valueint == 40);
    cJSON_Delete(object);
    CHECK(g_live == 0);

    // Every allocation of a whole tree failing in turn: nothing is left once the root is deleted
    reset_counts();
    cJSON *root = cJSON_CreateObject();
    build(root, 10);
    char *text = cJSON_PrintUnformatted(root); // the complete tree, for the control below
    cJSON_free(text);
    cJSON_Delete(root);
    CHECK(g_live == 0);
    reset_counts();
    root = cJSON_CreateObject();
    build(root, 10);
    const long total = g_allocations;
    cJSON_Delete(root);
    // root 1, two arrays with their keys 4, 10 texts of 2, 10 rows of 7 + an array of 8 with its key (10),
    // isr_bins 6 + 1 + key, tx_delay_min_ns 2 + 1 + key
    CHECK(total == 1 + 4 + 20 + 170 + 8 + 4);
    for (long fail = 0; fail < total; ++fail)
    {
        g_allocations = 0;
        g_fail_at = fail;
        root = cJSON_CreateObject();
        build(root, 10);
        cJSON_Delete(root);
        CHECK(g_live == 0);
    }
    // The control: with cJSON's own calls the same failures leave blocks allocated
    g_raw = true;
    long leaking = 0;
    for (long fail = 0; fail < total; ++fail)
    {
        g_allocations = 0;
        g_fail_at = fail;
        root = cJSON_CreateObject();
        build(root, 10);
        cJSON_Delete(root);
        leaking += g_live != 0 ? 1 : 0;
        for (void *block : g_blocks) // the blocks left
            free(block);
        g_blocks.clear();
        g_live = 0;
    }
    g_raw = false;
    CHECK(leaking > 0);
    g_fail_at = -1;
    cJSON_InitHooks(nullptr);
}
