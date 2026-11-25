// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE). From ESP32-WebTherm (core/json_tree.cpp of 76618ac).

#include "core/json_tree.h"

bool json_array_add(cJSON *array, cJSON *item)
{
    if (cJSON_AddItemToArray(array, item))
        return true;
    cJSON_Delete(item); // nullptr: nothing to delete
    return false;
}

bool json_object_add(cJSON *object, const char *name, cJSON *item)
{
    if (cJSON_AddItemToObject(object, name, item))
        return true;
    cJSON_Delete(item); // also frees the copy of the name if add_item_to_object() had set it
    return false;
}
