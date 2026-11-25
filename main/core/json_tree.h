// ESP32 Time Server
// Copyright Duglum, 2026. License: MIT (see LICENSE). From ESP32-WebTherm (core/json_tree.h of 76618ac), with
// json_object_add() for this project.
//
// Building a cJSON tree when memory may be short.
//
// cJSON_AddItemToArray() and cJSON_AddItemToObject() leave the item with the caller when they cannot add it: when the
// array or object is nullptr (it, or an object above it, could not be allocated), when the item is, or (for an object)
// when the copy of the name cannot be allocated (cJSON 1.7.19, add_item_to_array(), add_item_to_object()). An item
// built for a tree and not added was then left allocated, with what it held, and never zeroed. The
// cJSON_Add...ToObject() functions that create their own item (cJSON_AddStringToObject() and the like) delete it in
// that case; json_array_add() and json_object_add() do the same for an item built apart.

#pragma once

#include "cJSON.h"

// Adds item to array. When that is not possible, item is deleted (cJSON_Delete(): freed with all it holds, through the
// free function of the firmware, which zeroes first: sys/cjson_mem.cpp). True when it was added.
bool json_array_add(cJSON *array, cJSON *item);

// Adds item to object under name (copied). When that is not possible, item is deleted as above. True when it was added.
bool json_object_add(cJSON *object, const char *name, cJSON *item);
