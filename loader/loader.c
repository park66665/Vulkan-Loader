/*
 *
 * Copyright (c) 2014-2022 The Khronos Group Inc.
 * Copyright (c) 2014-2022 Valve Corporation
 * Copyright (c) 2014-2022 LunarG, Inc.
 * Copyright (C) 2015 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.

 *
 * Author: Jon Ashburn <jon@lunarg.com>
 * Author: Courtney Goeltzenleuchter <courtney@LunarG.com>
 * Author: Mark Young <marky@lunarg.com>
 * Author: Lenny Komow <lenny@lunarg.com>
 * Author: Charles Giessen <charles@lunarg.com>
 *
 */

#include "loader.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <sys/param.h>
#endif

// Time related functions
#include <time.h>

#include <sys/types.h>
#if defined(_WIN32)
#include "dirent_on_windows.h"
#else  // _WIN32
#include <dirent.h>
#endif  // _WIN32

#include "vulkan/vk_icd.h"

#include "allocation.h"
#include "cJSON.h"
#include "debug_utils.h"
#include "get_environment.h"
#include "gpa_helper.h"
#include "loader.h"
#include "log.h"
#include "murmurhash.h"
#include "vk_loader_platform.h"
#include "wsi.h"

#if defined(WIN32)
#include "loader_windows.h"
#endif
#ifdef LOADER_ENABLE_LINUX_SORT
// This header is currently only used when sorting Linux devices, so don't include it otherwise.
#include "loader_linux.h"
#endif  // LOADER_ENABLE_LINUX_SORT

// Generated file containing all the extension data
#include "vk_loader_extensions.c"

struct loader_struct loader = {0};

struct activated_layer_info {
    char *name;
    char *manifest;
    char *library;
    bool is_implicit;
    char *disable_env;
};

// thread safety lock for accessing global data structures such as "loader"
// all entrypoints on the instance chain need to be locked except GPA
// additionally CreateDevice and DestroyDevice needs to be locked
loader_platform_thread_mutex loader_lock;
loader_platform_thread_mutex loader_json_lock;
loader_platform_thread_mutex loader_preload_icd_lock;

// A list of ICDs that gets initialized when the loader does its global initialization. This list should never be used by anything
// other than EnumerateInstanceExtensionProperties(), vkDestroyInstance, and loader_release(). This list does not change
// functionality, but the fact that the libraries already been loaded causes any call that needs to load ICD libraries to speed up
// significantly. This can have a huge impact when making repeated calls to vkEnumerateInstanceExtensionProperties and
// vkCreateInstance.
static struct loader_icd_tramp_list scanned_icds;

LOADER_PLATFORM_THREAD_ONCE_DECLARATION(once_init);

// Wrapper around opendir so that the dirent_on_windows gets the instance it needs
// while linux opendir & readdir does not
DIR *loader_opendir(const struct loader_instance *instance, const char *name) {
#if defined(_WIN32)
    return opendir(instance, name);
#else  // _WIN32
    return opendir(name);

#endif  // _WIN32
}
int loader_closedir(const struct loader_instance *instance, DIR *dir) {
#if defined(_WIN32)
    return closedir(instance, dir);
#else   // _WIN32
    return closedir(dir);
#endif  // _WIN32
}

// log error from to library loading
void loader_log_load_library_error(const struct loader_instance *inst, const char *filename) {
    const char *error_message = loader_platform_open_library_error(filename);
    // If the error is due to incompatible architecture (eg 32 bit vs 64 bit), report it with INFO level
    // Discussed in Github issue 262 & 644
    // "wrong ELF class" is a linux error, " with error 193" is a windows error
    VkFlags err_flag = VULKAN_LOADER_ERROR_BIT;
    if (strstr(error_message, "wrong ELF class:") != NULL || strstr(error_message, " with error 193") != NULL) {
        err_flag = VULKAN_LOADER_INFO_BIT;
    }
    loader_log(inst, err_flag, 0, error_message);
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetInstanceDispatch(VkInstance instance, void *object) {
    struct loader_instance *inst = loader_get_instance(instance);
    if (!inst) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "vkSetInstanceDispatch: Can not retrieve Instance dispatch table.");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    loader_set_dispatch(object, inst->disp);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetDeviceDispatch(VkDevice device, void *object) {
    struct loader_device *dev;
    struct loader_icd_term *icd_term = loader_get_icd_and_device(device, &dev, NULL);

    if (NULL == icd_term) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    loader_set_dispatch(object, &dev->loader_dispatch);
    return VK_SUCCESS;
}

void loader_free_layer_properties(const struct loader_instance *inst, struct loader_layer_properties *layer_properties) {
    if (layer_properties->component_layer_names) {
        loader_instance_heap_free(inst, layer_properties->component_layer_names);
    }
    if (layer_properties->override_paths) {
        loader_instance_heap_free(inst, layer_properties->override_paths);
    }
    if (layer_properties->blacklist_layer_names) {
        loader_instance_heap_free(inst, layer_properties->blacklist_layer_names);
    }
    if (layer_properties->app_key_paths) {
        loader_instance_heap_free(inst, layer_properties->app_key_paths);
    }

    loader_destroy_generic_list(inst, (struct loader_generic_list *)&layer_properties->instance_extension_list);

    if (layer_properties->device_extension_list.capacity > 0 && NULL != layer_properties->device_extension_list.list) {
        for (uint32_t i = 0; i < layer_properties->device_extension_list.count; i++) {
            struct loader_dev_ext_props *ext_props = &layer_properties->device_extension_list.list[i];
            if (ext_props->entrypoint_count > 0) {
                for (uint32_t j = 0; j < ext_props->entrypoint_count; j++) {
                    loader_instance_heap_free(inst, ext_props->entrypoints[j]);
                }
                loader_instance_heap_free(inst, ext_props->entrypoints);
            }
        }
    }
    loader_destroy_generic_list(inst, (struct loader_generic_list *)&layer_properties->device_extension_list);

    // Make sure to clear out the removed layer, in case new layers are added in the previous location
    memset(layer_properties, 0, sizeof(struct loader_layer_properties));
}

// Combine path elements, separating each element with the platform-specific
// directory separator, and save the combined string to a destination buffer,
// not exceeding the given length. Path elements are given as variable args,
// with a NULL element terminating the list.
//
// \returns the total length of the combined string, not including an ASCII
// NUL termination character. This length may exceed the available storage:
// in this case, the written string will be truncated to avoid a buffer
// overrun, and the return value will greater than or equal to the storage
// size. A NULL argument may be provided as the destination buffer in order
// to determine the required string length without actually writing a string.
static size_t loader_platform_combine_path(char *dest, size_t len, ...) {
    size_t required_len = 0;
    va_list ap;
    const char *component;

    va_start(ap, len);

    while ((component = va_arg(ap, const char *))) {
        if (required_len > 0) {
            // This path element is not the first non-empty element; prepend
            // a directory separator if space allows
            if (dest && required_len + 1 < len) {
                (void)snprintf(dest + required_len, len - required_len, "%c", DIRECTORY_SYMBOL);
            }
            required_len++;
        }

        if (dest && required_len < len) {
            strncpy(dest + required_len, component, len - required_len);
        }
        required_len += strlen(component);
    }

    va_end(ap);

    // strncpy(3) won't add a NUL terminating byte in the event of truncation.
    if (dest && required_len >= len) {
        dest[len - 1] = '\0';
    }

    return required_len;
}

// Given string of three part form "maj.min.pat" convert to a vulkan version number.
static uint32_t loader_make_version(char *vers_str) {
    uint32_t major = 0, minor = 0, patch = 0;
    char *vers_tok;

    if (!vers_str) {
        return 0;
    }

    vers_tok = strtok(vers_str, ".\"\n\r");
    if (NULL != vers_tok) {
        major = (uint16_t)atoi(vers_tok);
        vers_tok = strtok(NULL, ".\"\n\r");
        if (NULL != vers_tok) {
            minor = (uint16_t)atoi(vers_tok);
            vers_tok = strtok(NULL, ".\"\n\r");
            if (NULL != vers_tok) {
                patch = (uint16_t)atoi(vers_tok);
            }
        }
    }

    return VK_MAKE_VERSION(major, minor, patch);
}

bool compare_vk_extension_properties(const VkExtensionProperties *op1, const VkExtensionProperties *op2) {
    return strcmp(op1->extensionName, op2->extensionName) == 0 ? true : false;
}

// Search the given ext_array for an extension matching the given vk_ext_prop
bool has_vk_extension_property_array(const VkExtensionProperties *vk_ext_prop, const uint32_t count,
                                     const VkExtensionProperties *ext_array) {
    for (uint32_t i = 0; i < count; i++) {
        if (compare_vk_extension_properties(vk_ext_prop, &ext_array[i])) return true;
    }
    return false;
}

// Search the given ext_list for an extension matching the given vk_ext_prop
bool has_vk_extension_property(const VkExtensionProperties *vk_ext_prop, const struct loader_extension_list *ext_list) {
    for (uint32_t i = 0; i < ext_list->count; i++) {
        if (compare_vk_extension_properties(&ext_list->list[i], vk_ext_prop)) return true;
    }
    return false;
}

// Search the given ext_list for a device extension matching the given ext_prop
bool has_vk_dev_ext_property(const VkExtensionProperties *ext_prop, const struct loader_device_extension_list *ext_list) {
    for (uint32_t i = 0; i < ext_list->count; i++) {
        if (compare_vk_extension_properties(&ext_list->list[i].props, ext_prop)) return true;
    }
    return false;
}

// Get the next unused layer property in the list. Init the property to zero.
static struct loader_layer_properties *loader_get_next_layer_property_slot(const struct loader_instance *inst,
                                                                           struct loader_layer_list *layer_list) {
    if (layer_list->capacity == 0) {
        layer_list->list =
            loader_instance_heap_alloc(inst, sizeof(struct loader_layer_properties) * 64, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
        if (layer_list->list == NULL) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_get_next_layer_property_slot: Out of memory can not add any layer properties to list");
            return NULL;
        }
        memset(layer_list->list, 0, sizeof(struct loader_layer_properties) * 64);
        layer_list->capacity = sizeof(struct loader_layer_properties) * 64;
    }

    // Ensure enough room to add an entry
    if ((layer_list->count + 1) * sizeof(struct loader_layer_properties) > layer_list->capacity) {
        void *new_ptr = loader_instance_heap_realloc(inst, layer_list->list, layer_list->capacity, layer_list->capacity * 2,
                                                     VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
        if (NULL == new_ptr) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_get_next_layer_property_slot: realloc failed for layer list");
            return NULL;
        }
        layer_list->list = new_ptr;
        memset((uint8_t *)layer_list->list + layer_list->capacity, 0, layer_list->capacity);
        layer_list->capacity *= 2;
    }

    layer_list->count++;
    return &(layer_list->list[layer_list->count - 1]);
}

// Search the given layer list for a layer property matching the given layer name
static struct loader_layer_properties *loader_find_layer_property(const char *name, const struct loader_layer_list *layer_list) {
    for (uint32_t i = 0; i < layer_list->count; i++) {
        const VkLayerProperties *item = &layer_list->list[i].info;
        if (strcmp(name, item->layerName) == 0) return &layer_list->list[i];
    }
    return NULL;
}

// Search the given layer list for a layer matching the given layer name
static bool loader_find_layer_name_in_list(const char *name, const struct loader_layer_list *layer_list) {
    if (NULL == layer_list) {
        return false;
    }
    if (NULL != loader_find_layer_property(name, layer_list)) {
        return true;
    }
    return false;
}

// Search the given meta-layer's component list for a layer matching the given layer name
static bool loader_find_layer_name_in_meta_layer(const struct loader_instance *inst, const char *layer_name,
                                                 struct loader_layer_list *layer_list,
                                                 struct loader_layer_properties *meta_layer_props) {
    for (uint32_t comp_layer = 0; comp_layer < meta_layer_props->num_component_layers; comp_layer++) {
        if (!strcmp(meta_layer_props->component_layer_names[comp_layer], layer_name)) {
            return true;
        }
        struct loader_layer_properties *comp_layer_props =
            loader_find_layer_property(meta_layer_props->component_layer_names[comp_layer], layer_list);
        if (comp_layer_props->type_flags & VK_LAYER_TYPE_FLAG_META_LAYER) {
            return loader_find_layer_name_in_meta_layer(inst, layer_name, layer_list, comp_layer_props);
        }
    }
    return false;
}

// Search the override layer's blacklist for a layer matching the given layer name
static bool loader_find_layer_name_in_blacklist(const struct loader_instance *inst, const char *layer_name,
                                                struct loader_layer_list *layer_list,
                                                struct loader_layer_properties *meta_layer_props) {
    for (uint32_t black_layer = 0; black_layer < meta_layer_props->num_blacklist_layers; ++black_layer) {
        if (!strcmp(meta_layer_props->blacklist_layer_names[black_layer], layer_name)) {
            return true;
        }
    }
    return false;
}

// Remove all layer properties entries from the list
void loader_delete_layer_list_and_properties(const struct loader_instance *inst, struct loader_layer_list *layer_list) {
    uint32_t i;
    if (!layer_list) return;

    for (i = 0; i < layer_list->count; i++) {
        loader_free_layer_properties(inst, &(layer_list->list[i]));
    }
    layer_list->count = 0;

    if (layer_list->capacity > 0) {
        layer_list->capacity = 0;
        loader_instance_heap_free(inst, layer_list->list);
    }
}

void loader_remove_layer_in_list(const struct loader_instance *inst, struct loader_layer_list *layer_list,
                                 uint32_t layer_to_remove) {
    if (layer_list == NULL || layer_to_remove >= layer_list->count) {
        return;
    }
    loader_free_layer_properties(inst, &(layer_list->list[layer_to_remove]));

    // Remove the current invalid meta-layer from the layer list.  Use memmove since we are
    // overlapping the source and destination addresses.
    memmove(&layer_list->list[layer_to_remove], &layer_list->list[layer_to_remove + 1],
            sizeof(struct loader_layer_properties) * (layer_list->count - 1 - layer_to_remove));

    // Decrement the count (because we now have one less) and decrement the loop index since we need to
    // re-check this index.
    layer_list->count--;
}

// Remove all layers in the layer list that are blacklisted by the override layer.
// NOTE: This should only be called if an override layer is found and not expired.
void loader_remove_layers_in_blacklist(const struct loader_instance *inst, struct loader_layer_list *layer_list) {
    struct loader_layer_properties *override_prop = loader_find_layer_property(VK_OVERRIDE_LAYER_NAME, layer_list);
    if (NULL == override_prop) {
        return;
    }

    for (int32_t j = 0; j < (int32_t)(layer_list->count); j++) {
        struct loader_layer_properties cur_layer_prop = layer_list->list[j];
        const char *cur_layer_name = &cur_layer_prop.info.layerName[0];

        // Skip the override layer itself.
        if (!strcmp(VK_OVERRIDE_LAYER_NAME, cur_layer_name)) {
            continue;
        }

        // If found in the override layer's blacklist, remove it
        if (loader_find_layer_name_in_blacklist(inst, cur_layer_name, layer_list, override_prop)) {
            loader_log(inst, VULKAN_LOADER_DEBUG_BIT, 0,
                       "loader_remove_layers_in_blacklist: Override layer is active and layer %s is in the blacklist inside of it. "
                       "Removing that layer from current layer list.",
                       cur_layer_name);
            loader_remove_layer_in_list(inst, layer_list, j);
            j--;

            // Re-do the query for the override layer
            override_prop = loader_find_layer_property(VK_OVERRIDE_LAYER_NAME, layer_list);
        }
    }
}

// Remove all layers in the layer list that are not found inside any implicit meta-layers.
void loader_remove_layers_not_in_implicit_meta_layers(const struct loader_instance *inst, struct loader_layer_list *layer_list) {
    int32_t i;
    int32_t j;
    int32_t layer_count = (int32_t)(layer_list->count);

    for (i = 0; i < layer_count; i++) {
        layer_list->list[i].keep = false;
    }

    for (i = 0; i < layer_count; i++) {
        struct loader_layer_properties *cur_layer_prop = &layer_list->list[i];

        if (0 == (cur_layer_prop->type_flags & VK_LAYER_TYPE_FLAG_EXPLICIT_LAYER)) {
            cur_layer_prop->keep = true;
            continue;
        }
        for (j = 0; j < layer_count; j++) {
            struct loader_layer_properties layer_to_check = layer_list->list[j];

            if (i == j) {
                continue;
            }

            if (layer_to_check.type_flags & VK_LAYER_TYPE_FLAG_META_LAYER) {
                // For all layers found in this meta layer, we want to keep them as well.
                if (loader_find_layer_name_in_meta_layer(inst, cur_layer_prop->info.layerName, layer_list, &layer_to_check)) {
                    cur_layer_prop->keep = true;
                }
            }
        }
    }

    // Remove any layers we don't want to keep (Don't use layer_count here as we need it to be
    // dynamically updated if we delete a layer property in the list).
    for (i = 0; i < (int32_t)(layer_list->count); i++) {
        struct loader_layer_properties cur_layer_prop = layer_list->list[i];
        if (!cur_layer_prop.keep) {
            loader_log(
                inst, VULKAN_LOADER_DEBUG_BIT, 0,
                "loader_remove_layers_not_in_implicit_meta_layers : Implicit meta-layers are active, and layer %s is not list "
                "inside of any.  So removing layer from current layer list.",
                cur_layer_prop.info.layerName);
            loader_remove_layer_in_list(inst, layer_list, i);
            i--;
        }
    }
}

static VkResult loader_add_instance_extensions(const struct loader_instance *inst,
                                               const PFN_vkEnumerateInstanceExtensionProperties fp_get_props, const char *lib_name,
                                               struct loader_extension_list *ext_list) {
    uint32_t i, count = 0;
    VkExtensionProperties *ext_props;
    VkResult res = VK_SUCCESS;

    if (!fp_get_props) {
        // No EnumerateInstanceExtensionProperties defined
        goto out;
    }

    res = fp_get_props(NULL, &count, NULL);
    if (res != VK_SUCCESS) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "loader_add_instance_extensions: Error getting Instance extension count from %s", lib_name);
        goto out;
    }

    if (count == 0) {
        // No ExtensionProperties to report
        goto out;
    }

    ext_props = loader_stack_alloc(count * sizeof(VkExtensionProperties));
    if (NULL == ext_props) {
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }

    res = fp_get_props(NULL, &count, ext_props);
    if (res != VK_SUCCESS) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_add_instance_extensions: Error getting Instance extensions from %s",
                   lib_name);
        goto out;
    }

    for (i = 0; i < count; i++) {
        char spec_version[64];

        bool ext_unsupported = wsi_unsupported_instance_extension(&ext_props[i]);
        if (!ext_unsupported) {
            (void)snprintf(spec_version, sizeof(spec_version), "%d.%d.%d", VK_VERSION_MAJOR(ext_props[i].specVersion),
                           VK_VERSION_MINOR(ext_props[i].specVersion), VK_VERSION_PATCH(ext_props[i].specVersion));
            loader_log(inst, VULKAN_LOADER_DEBUG_BIT, 0, "Instance Extension: %s (%s) version %s", ext_props[i].extensionName,
                       lib_name, spec_version);

            res = loader_add_to_ext_list(inst, ext_list, 1, &ext_props[i]);
            if (res != VK_SUCCESS) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "loader_add_instance_extensions: Failed to add %s to Instance extension list", lib_name);
                goto out;
            }
        }
    }

out:
    return res;
}

// Initialize ext_list with the physical device extensions.
// The extension properties are passed as inputs in count and ext_props.
static VkResult loader_init_device_extensions(const struct loader_instance *inst, struct loader_physical_device_term *phys_dev_term,
                                              uint32_t count, VkExtensionProperties *ext_props,
                                              struct loader_extension_list *ext_list) {
    VkResult res;
    uint32_t i;

    res = loader_init_generic_list(inst, (struct loader_generic_list *)ext_list, sizeof(VkExtensionProperties));
    if (VK_SUCCESS != res) {
        return res;
    }

    for (i = 0; i < count; i++) {
        char spec_version[64];
        (void)snprintf(spec_version, sizeof(spec_version), "%d.%d.%d", VK_VERSION_MAJOR(ext_props[i].specVersion),
                       VK_VERSION_MINOR(ext_props[i].specVersion), VK_VERSION_PATCH(ext_props[i].specVersion));
        loader_log(inst, VULKAN_LOADER_DEBUG_BIT, 0, "Device Extension: %s (%s) version %s", ext_props[i].extensionName,
                   phys_dev_term->this_icd_term->scanned_icd->lib_name, spec_version);
        res = loader_add_to_ext_list(inst, ext_list, 1, &ext_props[i]);
        if (res != VK_SUCCESS) return res;
    }

    return VK_SUCCESS;
}

VkResult loader_add_device_extensions(const struct loader_instance *inst,
                                      PFN_vkEnumerateDeviceExtensionProperties fpEnumerateDeviceExtensionProperties,
                                      VkPhysicalDevice physical_device, const char *lib_name,
                                      struct loader_extension_list *ext_list) {
    uint32_t i = 0, count = 0;
    VkResult res = VK_SUCCESS;
    VkExtensionProperties *ext_props = NULL;

    res = fpEnumerateDeviceExtensionProperties(physical_device, NULL, &count, NULL);
    if (res == VK_SUCCESS && count > 0) {
        ext_props = loader_stack_alloc(count * sizeof(VkExtensionProperties));
        if (!ext_props) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_add_device_extensions: Failed to allocate space for device extension properties.");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        res = fpEnumerateDeviceExtensionProperties(physical_device, NULL, &count, ext_props);
        if (res != VK_SUCCESS) {
            return res;
        }
        for (i = 0; i < count; i++) {
            char spec_version[64];
            (void)snprintf(spec_version, sizeof(spec_version), "%d.%d.%d", VK_VERSION_MAJOR(ext_props[i].specVersion),
                           VK_VERSION_MINOR(ext_props[i].specVersion), VK_VERSION_PATCH(ext_props[i].specVersion));
            loader_log(inst, VULKAN_LOADER_DEBUG_BIT, 0, "Device Extension: %s (%s) version %s", ext_props[i].extensionName,
                       lib_name, spec_version);
            res = loader_add_to_ext_list(inst, ext_list, 1, &ext_props[i]);
            if (res != VK_SUCCESS) {
                return res;
            }
        }
    } else {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "loader_add_device_extensions: Error getting physical device extension info count from library %s", lib_name);
        return res;
    }

    return VK_SUCCESS;
}

VkResult loader_init_generic_list(const struct loader_instance *inst, struct loader_generic_list *list_info, size_t element_size) {
    size_t capacity = 32 * element_size;
    list_info->count = 0;
    list_info->capacity = 0;
    list_info->list = loader_instance_heap_alloc(inst, capacity, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    if (list_info->list == NULL) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_init_generic_list: Failed to allocate space for generic list");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memset(list_info->list, 0, capacity);
    list_info->capacity = capacity;
    return VK_SUCCESS;
}

void loader_destroy_generic_list(const struct loader_instance *inst, struct loader_generic_list *list) {
    loader_instance_heap_free(inst, list->list);
    list->count = 0;
    list->capacity = 0;
}

// Append non-duplicate extension properties defined in props to the given ext_list.
// Return - Vk_SUCCESS on success
VkResult loader_add_to_ext_list(const struct loader_instance *inst, struct loader_extension_list *ext_list,
                                uint32_t prop_list_count, const VkExtensionProperties *props) {
    uint32_t i;
    const VkExtensionProperties *cur_ext;

    if (ext_list->list == NULL || ext_list->capacity == 0) {
        VkResult res = loader_init_generic_list(inst, (struct loader_generic_list *)ext_list, sizeof(VkExtensionProperties));
        if (VK_SUCCESS != res) {
            return res;
        }
    }

    for (i = 0; i < prop_list_count; i++) {
        cur_ext = &props[i];

        // look for duplicates
        if (has_vk_extension_property(cur_ext, ext_list)) {
            continue;
        }

        // add to list at end
        // check for enough capacity
        if (ext_list->count * sizeof(VkExtensionProperties) >= ext_list->capacity) {
            void *new_ptr = loader_instance_heap_realloc(inst, ext_list->list, ext_list->capacity, ext_list->capacity * 2,
                                                         VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
            if (new_ptr == NULL) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "loader_add_to_ext_list: Failed to reallocate space for extension list");
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            ext_list->list = new_ptr;

            // double capacity
            ext_list->capacity *= 2;
        }

        memcpy(&ext_list->list[ext_list->count], cur_ext, sizeof(VkExtensionProperties));
        ext_list->count++;
    }
    return VK_SUCCESS;
}

// Append one extension property defined in props with entrypoints defined in entries to the given
// ext_list. Do not append if a duplicate.
// Return - Vk_SUCCESS on success
VkResult loader_add_to_dev_ext_list(const struct loader_instance *inst, struct loader_device_extension_list *ext_list,
                                    const VkExtensionProperties *props, uint32_t entry_count, char **entrys) {
    uint32_t idx;
    if (ext_list->list == NULL || ext_list->capacity == 0) {
        VkResult res = loader_init_generic_list(inst, (struct loader_generic_list *)ext_list, sizeof(struct loader_dev_ext_props));
        if (VK_SUCCESS != res) {
            return res;
        }
    }

    // look for duplicates
    if (has_vk_dev_ext_property(props, ext_list)) {
        return VK_SUCCESS;
    }

    idx = ext_list->count;
    // add to list at end
    // check for enough capacity
    if (idx * sizeof(struct loader_dev_ext_props) >= ext_list->capacity) {
        void *new_ptr = loader_instance_heap_realloc(inst, ext_list->list, ext_list->capacity, ext_list->capacity * 2,
                                                     VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);

        if (NULL == new_ptr) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_add_to_dev_ext_list: Failed to reallocate space for device extension list");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        ext_list->list = new_ptr;

        // double capacity
        ext_list->capacity *= 2;
    }

    memcpy(&ext_list->list[idx].props, props, sizeof(*props));
    ext_list->list[idx].entrypoint_count = entry_count;
    if (entry_count == 0) {
        ext_list->list[idx].entrypoints = NULL;
    } else {
        ext_list->list[idx].entrypoints =
            loader_instance_heap_alloc(inst, sizeof(char *) * entry_count, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
        if (ext_list->list[idx].entrypoints == NULL) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_add_to_dev_ext_list: Failed to allocate space for device extension entrypoint list in list %d", idx);
            ext_list->list[idx].entrypoint_count = 0;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        for (uint32_t i = 0; i < entry_count; i++) {
            ext_list->list[idx].entrypoints[i] =
                loader_instance_heap_alloc(inst, strlen(entrys[i]) + 1, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
            if (ext_list->list[idx].entrypoints[i] == NULL) {
                for (uint32_t j = 0; j < i; j++) {
                    loader_instance_heap_free(inst, ext_list->list[idx].entrypoints[j]);
                }
                loader_instance_heap_free(inst, ext_list->list[idx].entrypoints);
                ext_list->list[idx].entrypoint_count = 0;
                ext_list->list[idx].entrypoints = NULL;
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "loader_add_to_dev_ext_list: Failed to allocate space for device extension entrypoint %d name", i);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            strcpy(ext_list->list[idx].entrypoints[i], entrys[i]);
        }
    }
    ext_list->count++;

    return VK_SUCCESS;
}

// Prototypes needed.
bool loader_add_meta_layer(const struct loader_instance *inst, const struct loader_layer_properties *prop,
                           struct loader_layer_list *target_list, struct loader_layer_list *expanded_target_list,
                           const struct loader_layer_list *source_list);

// Manage lists of VkLayerProperties
static bool loader_init_layer_list(const struct loader_instance *inst, struct loader_layer_list *list) {
    list->capacity = 32 * sizeof(struct loader_layer_properties);
    list->list = loader_instance_heap_alloc(inst, list->capacity, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    if (list->list == NULL) {
        return false;
    }
    memset(list->list, 0, list->capacity);
    list->count = 0;
    return true;
}

// Search the given array of layer names for an entry matching the given VkLayerProperties
bool loader_names_array_has_layer_property(const VkLayerProperties *vk_layer_prop, uint32_t layer_info_count,
                                           struct activated_layer_info *layer_info) {
    for (uint32_t i = 0; i < layer_info_count; i++) {
        if (strcmp(vk_layer_prop->layerName, layer_info[i].name) == 0) {
            return true;
        }
    }
    return false;
}

void loader_destroy_layer_list(const struct loader_instance *inst, struct loader_device *device,
                               struct loader_layer_list *layer_list) {
    if (device) {
        loader_device_heap_free(device, layer_list->list);
    } else {
        loader_instance_heap_free(inst, layer_list->list);
    }
    layer_list->count = 0;
    layer_list->capacity = 0;
}

// Append layer properties defined in prop_list to the given layer_info list
VkResult loader_add_layer_properties_to_list(const struct loader_instance *inst, struct loader_layer_list *list,
                                             uint32_t prop_list_count, const struct loader_layer_properties *props) {
    uint32_t i;
    struct loader_layer_properties *layer;

    if (list->list == NULL || list->capacity == 0) {
        if (!loader_init_layer_list(inst, list)) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    if (list->list == NULL) return VK_SUCCESS;

    for (i = 0; i < prop_list_count; i++) {
        layer = (struct loader_layer_properties *)&props[i];

        // Check for enough capacity
        if (((list->count + 1) * sizeof(struct loader_layer_properties)) >= list->capacity) {
            size_t new_capacity = list->capacity * 2;
            void *new_ptr =
                loader_instance_heap_realloc(inst, list->list, list->capacity, new_capacity, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
            if (NULL == new_ptr) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "loader_add_layer_properties_to_list: Realloc failed for when attempting to add new layer");
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            list->list = new_ptr;
            list->capacity = new_capacity;
        }

        memcpy(&list->list[list->count], layer, sizeof(struct loader_layer_properties));
        list->count++;
    }

    return VK_SUCCESS;
}

// Search the given search_list for any layers in the props list.  Add these to the
// output layer_list.
static VkResult loader_add_layer_names_to_list(const struct loader_instance *inst, struct loader_layer_list *output_list,
                                               struct loader_layer_list *expanded_output_list, uint32_t name_count,
                                               const char *const *names, const struct loader_layer_list *source_list) {
    struct loader_layer_properties *layer_prop;
    VkResult err = VK_SUCCESS;

    for (uint32_t i = 0; i < name_count; i++) {
        const char *source_name = names[i];
        layer_prop = loader_find_layer_property(source_name, source_list);
        if (NULL == layer_prop) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                       "loader_add_layer_names_to_list: Unable to find layer %s", source_name);
            err = VK_ERROR_LAYER_NOT_PRESENT;
            continue;
        }

        // Make sure the layer isn't already in the output_list, skip adding it if it is.
        if (loader_find_layer_name_in_list(source_name, output_list)) {
            continue;
        }

        // If not a meta-layer, simply add it.
        if (0 == (layer_prop->type_flags & VK_LAYER_TYPE_FLAG_META_LAYER)) {
            loader_add_layer_properties_to_list(inst, output_list, 1, layer_prop);
            loader_add_layer_properties_to_list(inst, expanded_output_list, 1, layer_prop);
        } else {
            loader_add_meta_layer(inst, layer_prop, output_list, expanded_output_list, source_list);
        }
    }

    return err;
}

static bool check_expiration(const struct loader_instance *inst, const struct loader_layer_properties *prop) {
    time_t current = time(NULL);
    struct tm tm_current = *localtime(&current);

    struct tm tm_expiration = {
        .tm_sec = 0,
        .tm_min = prop->expiration.minute,
        .tm_hour = prop->expiration.hour,
        .tm_mday = prop->expiration.day,
        .tm_mon = prop->expiration.month - 1,
        .tm_year = prop->expiration.year - 1900,
        .tm_isdst = tm_current.tm_isdst,
        // wday and yday are ignored by mktime
    };
    time_t expiration = mktime(&tm_expiration);

    return current < expiration;
}

// Determine if the provided implicit layer should be enabled by querying the appropriate environmental variables.
// For an implicit layer, at least a disable environment variable is required.
bool loader_implicit_layer_is_enabled(const struct loader_instance *inst, const struct loader_layer_properties *prop) {
    bool enable = false;
    char *env_value = NULL;

    // If no enable_environment variable is specified, this implicit layer is always be enabled by default.
    if (prop->enable_env_var.name[0] == 0) {
        enable = true;
    } else {
        // Otherwise, only enable this layer if the enable environment variable is defined
        env_value = loader_getenv(prop->enable_env_var.name, inst);
        if (env_value && !strcmp(prop->enable_env_var.value, env_value)) {
            enable = true;
        }
        loader_free_getenv(env_value, inst);
    }

    // The disable_environment has priority over everything else.  If it is defined, the layer is always
    // disabled.
    env_value = loader_getenv(prop->disable_env_var.name, inst);
    if (env_value) {
        enable = false;
    }
    loader_free_getenv(env_value, inst);

    // If this layer has an expiration, check it to determine if this layer has expired.
    if (prop->has_expiration) {
        enable = check_expiration(inst, prop);
    }

    // Enable this layer if it is included in the override layer
    if (inst != NULL && inst->override_layer_present) {
        struct loader_layer_properties *override = NULL;
        for (uint32_t i = 0; i < inst->instance_layer_list.count; ++i) {
            if (strcmp(inst->instance_layer_list.list[i].info.layerName, VK_OVERRIDE_LAYER_NAME) == 0) {
                override = &inst->instance_layer_list.list[i];
                break;
            }
        }
        if (override != NULL) {
            for (uint32_t i = 0; i < override->num_component_layers; ++i) {
                if (strcmp(override->component_layer_names[i], prop->info.layerName) == 0) {
                    enable = true;
                    break;
                }
            }
        }
    }

    return enable;
}

// Check the individual implicit layer for the enable/disable environment variable settings.  Only add it after
// every check has passed indicating it should be used.
static void loader_add_implicit_layer(const struct loader_instance *inst, const struct loader_layer_properties *prop,
                                      struct loader_layer_list *target_list, struct loader_layer_list *expanded_target_list,
                                      const struct loader_layer_list *source_list) {
    bool enable = loader_implicit_layer_is_enabled(inst, prop);

    // If the implicit layer is supposed to be enable, make sure the layer supports at least the same API version
    // that the application is asking (i.e. layer's API >= app's API).  If it's not, disable this layer.
    if (enable) {
        uint16_t layer_api_major_version = VK_VERSION_MAJOR(prop->info.specVersion);
        uint16_t layer_api_minor_version = VK_VERSION_MINOR(prop->info.specVersion);
        if (inst->app_api_major_version > layer_api_major_version ||
            (inst->app_api_major_version == layer_api_major_version && inst->app_api_minor_version > layer_api_minor_version)) {
            loader_log(inst, VULKAN_LOADER_INFO_BIT, 0,
                       "loader_add_implicit_layer: Disabling implicit layer %s for using an old API version %d.%d versus "
                       "application requested %d.%d",
                       prop->info.layerName, layer_api_major_version, layer_api_minor_version, inst->app_api_major_version,
                       inst->app_api_minor_version);
            enable = false;
        }
    }

    if (enable) {
        if (0 == (prop->type_flags & VK_LAYER_TYPE_FLAG_META_LAYER)) {
            loader_add_layer_properties_to_list(inst, target_list, 1, prop);
            if (NULL != expanded_target_list) {
                loader_add_layer_properties_to_list(inst, expanded_target_list, 1, prop);
            }
        } else {
            loader_add_meta_layer(inst, prop, target_list, expanded_target_list, source_list);
        }
    }
}

// Add the component layers of a meta-layer to the active list of layers
bool loader_add_meta_layer(const struct loader_instance *inst, const struct loader_layer_properties *prop,
                           struct loader_layer_list *target_list, struct loader_layer_list *expanded_target_list,
                           const struct loader_layer_list *source_list) {
    bool found = true;

    // We need to add all the individual component layers
    uint16_t meta_layer_api_major_version = VK_VERSION_MAJOR(prop->info.specVersion);
    uint16_t meta_layer_api_minor_version = VK_VERSION_MINOR(prop->info.specVersion);
    for (uint32_t comp_layer = 0; comp_layer < prop->num_component_layers; comp_layer++) {
        bool found_comp = false;
        const struct loader_layer_properties *search_prop =
            loader_find_layer_property(prop->component_layer_names[comp_layer], source_list);
        if (search_prop != NULL) {
            found_comp = true;

            uint16_t search_layer_api_major_version = VK_VERSION_MAJOR(search_prop->info.specVersion);
            uint16_t search_layer_api_minor_version = VK_VERSION_MINOR(search_prop->info.specVersion);
            if (meta_layer_api_major_version != search_layer_api_major_version ||
                meta_layer_api_minor_version > search_layer_api_minor_version) {
                loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                           "loader_add_meta_layer: Meta-layer API version %u.%u, component layer %s version %u.%u, may have "
                           "incompatibilities (Policy #LLP_LAYER_8)!",
                           meta_layer_api_major_version, meta_layer_api_minor_version, search_prop->info.layerName,
                           search_layer_api_major_version, meta_layer_api_minor_version);
            }

            // If the component layer is itself an implicit layer, we need to do the implicit layer enable
            // checks
            if (0 == (search_prop->type_flags & VK_LAYER_TYPE_FLAG_EXPLICIT_LAYER)) {
                loader_add_implicit_layer(inst, search_prop, target_list, expanded_target_list, source_list);
            } else {
                if (0 != (search_prop->type_flags & VK_LAYER_TYPE_FLAG_META_LAYER)) {
                    found = loader_add_meta_layer(inst, search_prop, target_list, expanded_target_list, source_list);
                } else {
                    loader_add_layer_properties_to_list(inst, target_list, 1, search_prop);
                    if (NULL != expanded_target_list) {
                        loader_add_layer_properties_to_list(inst, expanded_target_list, 1, search_prop);
                    }
                }
            }
        }
        if (!found_comp) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                       "loader_add_meta_layer: Failed to find layer name %s component layer %s to activate (Policy #LLP_LAYER_7)",
                       search_prop->info.layerName, prop->component_layer_names[comp_layer]);
            found = false;
        }
    }

    // Add this layer to the overall target list (not the expanded one)
    if (found) {
        loader_add_layer_properties_to_list(inst, target_list, 1, prop);
    }

    return found;
}

// Search the source_list for any layer with a name that matches the given name and a type
// that matches the given type.  Add all matching layers to the target_list.
VkResult loader_add_layer_name_to_list(const struct loader_instance *inst, const char *name, const enum layer_type_flags type_flags,
                                       const struct loader_layer_list *source_list, struct loader_layer_list *target_list,
                                       struct loader_layer_list *expanded_target_list) {
    VkResult res = VK_SUCCESS;
    bool found = false;
    for (uint32_t i = 0; i < source_list->count; i++) {
        struct loader_layer_properties *source_prop = &source_list->list[i];
        if (0 == strcmp(source_prop->info.layerName, name) && (source_prop->type_flags & type_flags) == type_flags) {
            // If not a meta-layer, simply add it.
            if (0 == (source_prop->type_flags & VK_LAYER_TYPE_FLAG_META_LAYER)) {
                if (VK_SUCCESS == loader_add_layer_properties_to_list(inst, target_list, 1, source_prop)) {
                    found = true;
                }
                if (VK_SUCCESS == loader_add_layer_properties_to_list(inst, expanded_target_list, 1, source_prop)) {
                    found = true;
                }
            } else {
                found = loader_add_meta_layer(inst, source_prop, target_list, expanded_target_list, source_list);
            }
        }
    }
    if (!found) {
        if (strcmp(name, "VK_LAYER_LUNARG_standard_validation")) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                       "loader_add_layer_name_to_list: Failed to find layer name %s to activate", name);
        } else {
            res = VK_ERROR_LAYER_NOT_PRESENT;
            loader_log(inst, VULKAN_LOADER_ERROR_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                       "Layer VK_LAYER_LUNARG_standard_validation has been changed to VK_LAYER_KHRONOS_validation. Please use the "
                       "new version of the layer.");
        }
    }
    return res;
}

static VkExtensionProperties *get_extension_property(const char *name, const struct loader_extension_list *list) {
    for (uint32_t i = 0; i < list->count; i++) {
        if (strcmp(name, list->list[i].extensionName) == 0) return &list->list[i];
    }
    return NULL;
}

static VkExtensionProperties *get_dev_extension_property(const char *name, const struct loader_device_extension_list *list) {
    for (uint32_t i = 0; i < list->count; i++) {
        if (strcmp(name, list->list[i].props.extensionName) == 0) return &list->list[i].props;
    }
    return NULL;
}

// For Instance extensions implemented within the loader (i.e. DEBUG_REPORT
// the extension must provide two entry points for the loader to use:
// - "trampoline" entry point - this is the address returned by GetProcAddr
//                              and will always do what's necessary to support a
//                              global call.
// - "terminator" function    - this function will be put at the end of the
//                              instance chain and will contain the necessary logic
//                              to call / process the extension for the appropriate
//                              ICDs that are available.
// There is no generic mechanism for including these functions, the references
// must be placed into the appropriate loader entry points.
// GetInstanceProcAddr: call extension GetInstanceProcAddr to check for GetProcAddr
// requests
// loader_coalesce_extensions(void) - add extension records to the list of global
//                                    extension available to the app.
// instance_disp                    - add function pointer for terminator function
//                                    to this array.
// The extension itself should be in a separate file that will be linked directly
// with the loader.
VkResult loader_get_icd_loader_instance_extensions(const struct loader_instance *inst, struct loader_icd_tramp_list *icd_tramp_list,
                                                   struct loader_extension_list *inst_exts) {
    struct loader_extension_list icd_exts;
    VkResult res = VK_SUCCESS;
    char *env_value;
    bool filter_extensions = true;

    loader_log(inst, VULKAN_LOADER_DEBUG_BIT, 0, "Build ICD instance extension list");

    // Check if a user wants to disable the instance extension filtering behavior
    env_value = loader_getenv("VK_LOADER_DISABLE_INST_EXT_FILTER", inst);
    if (NULL != env_value && atoi(env_value) != 0) {
        filter_extensions = false;
    }
    loader_free_getenv(env_value, inst);

    // traverse scanned icd list adding non-duplicate extensions to the list
    for (uint32_t i = 0; i < icd_tramp_list->count; i++) {
        res = loader_init_generic_list(inst, (struct loader_generic_list *)&icd_exts, sizeof(VkExtensionProperties));
        if (VK_SUCCESS != res) {
            goto out;
        }
        res = loader_add_instance_extensions(inst, icd_tramp_list->scanned_list[i].EnumerateInstanceExtensionProperties,
                                             icd_tramp_list->scanned_list[i].lib_name, &icd_exts);
        if (VK_SUCCESS == res) {
            if (filter_extensions) {
                // Remove any extensions not recognized by the loader
                for (int32_t j = 0; j < (int32_t)icd_exts.count; j++) {
                    // See if the extension is in the list of supported extensions
                    bool found = false;
                    for (uint32_t k = 0; LOADER_INSTANCE_EXTENSIONS[k] != NULL; k++) {
                        if (strcmp(icd_exts.list[j].extensionName, LOADER_INSTANCE_EXTENSIONS[k]) == 0) {
                            found = true;
                            break;
                        }
                    }

                    // If it isn't in the list, remove it
                    if (!found) {
                        for (uint32_t k = j + 1; k < icd_exts.count; k++) {
                            icd_exts.list[k - 1] = icd_exts.list[k];
                        }
                        --icd_exts.count;
                        --j;
                    }
                }
            }

            res = loader_add_to_ext_list(inst, inst_exts, icd_exts.count, icd_exts.list);
        }
        loader_destroy_generic_list(inst, (struct loader_generic_list *)&icd_exts);
        if (VK_SUCCESS != res) {
            goto out;
        }
    };

    // Traverse loader's extensions, adding non-duplicate extensions to the list
    debug_utils_AddInstanceExtensions(inst, inst_exts);

out:
    return res;
}

struct loader_icd_term *loader_get_icd_and_device(const void *device, struct loader_device **found_dev, uint32_t *icd_index) {
    *found_dev = NULL;
    for (struct loader_instance *inst = loader.instances; inst; inst = inst->next) {
        uint32_t index = 0;
        for (struct loader_icd_term *icd_term = inst->icd_terms; icd_term; icd_term = icd_term->next) {
            for (struct loader_device *dev = icd_term->logical_device_list; dev; dev = dev->next)
                // Value comparison of device prevents object wrapping by layers
                if (loader_get_dispatch(dev->icd_device) == loader_get_dispatch(device) ||
                    (dev->chain_device != VK_NULL_HANDLE &&
                     loader_get_dispatch(dev->chain_device) == loader_get_dispatch(device))) {
                    *found_dev = dev;
                    if (NULL != icd_index) {
                        *icd_index = index;
                    }
                    return icd_term;
                }
            index++;
        }
    }
    return NULL;
}

void loader_destroy_logical_device(const struct loader_instance *inst, struct loader_device *dev,
                                   const VkAllocationCallbacks *pAllocator) {
    if (pAllocator) {
        dev->alloc_callbacks = *pAllocator;
    }
    if (NULL != dev->expanded_activated_layer_list.list) {
        loader_deactivate_layers(inst, dev, &dev->expanded_activated_layer_list);
    }
    if (NULL != dev->app_activated_layer_list.list) {
        loader_destroy_layer_list(inst, dev, &dev->app_activated_layer_list);
    }
    loader_device_heap_free(dev, dev);
}

struct loader_device *loader_create_logical_device(const struct loader_instance *inst, const VkAllocationCallbacks *pAllocator) {
    struct loader_device *new_dev;
#if (DEBUG_DISABLE_APP_ALLOCATORS == 1)
    {
#else
    if (pAllocator) {
        new_dev = (struct loader_device *)pAllocator->pfnAllocation(pAllocator->pUserData, sizeof(struct loader_device),
                                                                    sizeof(int *), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    } else {
#endif
        new_dev = (struct loader_device *)malloc(sizeof(struct loader_device));
    }

    if (!new_dev) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_create_logical_device: Failed to alloc struct loader_device");
        return NULL;
    }

    memset(new_dev, 0, sizeof(struct loader_device));
    if (pAllocator) {
        new_dev->alloc_callbacks = *pAllocator;
    }

    return new_dev;
}

void loader_add_logical_device(const struct loader_instance *inst, struct loader_icd_term *icd_term, struct loader_device *dev) {
    dev->next = icd_term->logical_device_list;
    icd_term->logical_device_list = dev;
}

void loader_remove_logical_device(const struct loader_instance *inst, struct loader_icd_term *icd_term,
                                  struct loader_device *found_dev, const VkAllocationCallbacks *pAllocator) {
    struct loader_device *dev, *prev_dev;

    if (!icd_term || !found_dev) return;

    prev_dev = NULL;
    dev = icd_term->logical_device_list;
    while (dev && dev != found_dev) {
        prev_dev = dev;
        dev = dev->next;
    }

    if (prev_dev)
        prev_dev->next = found_dev->next;
    else
        icd_term->logical_device_list = found_dev->next;
    loader_destroy_logical_device(inst, found_dev, pAllocator);
}

static void loader_icd_destroy(struct loader_instance *ptr_inst, struct loader_icd_term *icd_term,
                               const VkAllocationCallbacks *pAllocator) {
    ptr_inst->total_icd_count--;
    for (struct loader_device *dev = icd_term->logical_device_list; dev;) {
        struct loader_device *next_dev = dev->next;
        loader_destroy_logical_device(ptr_inst, dev, pAllocator);
        dev = next_dev;
    }

    loader_instance_heap_free(ptr_inst, icd_term);
}

static struct loader_icd_term *loader_icd_create(const struct loader_instance *inst) {
    struct loader_icd_term *icd_term;

    icd_term = loader_instance_heap_alloc(inst, sizeof(struct loader_icd_term), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    if (!icd_term) {
        return NULL;
    }

    memset(icd_term, 0, sizeof(struct loader_icd_term));

    return icd_term;
}

static struct loader_icd_term *loader_icd_add(struct loader_instance *ptr_inst, const struct loader_scanned_icd *scanned_icd) {
    struct loader_icd_term *icd_term;

    icd_term = loader_icd_create(ptr_inst);
    if (!icd_term) {
        return NULL;
    }

    icd_term->scanned_icd = scanned_icd;
    icd_term->this_instance = ptr_inst;

    // Prepend to the list
    icd_term->next = ptr_inst->icd_terms;
    ptr_inst->icd_terms = icd_term;
    ptr_inst->total_icd_count++;

    return icd_term;
}

// Determine the ICD interface version to use.
//     @param icd
//     @param pVersion Output parameter indicating which version to use or 0 if
//            the negotiation API is not supported by the ICD
//     @return  bool indicating true if the selected interface version is supported
//            by the loader, false indicates the version is not supported
bool loader_get_icd_interface_version(PFN_vkNegotiateLoaderICDInterfaceVersion fp_negotiate_icd_version, uint32_t *pVersion) {
    if (fp_negotiate_icd_version == NULL) {
        // ICD does not support the negotiation API, it supports version 0 or 1
        // calling code must determine if it is version 0 or 1
        *pVersion = 0;
    } else {
        // ICD supports the negotiation API, so call it with the loader's
        // latest version supported
        *pVersion = CURRENT_LOADER_ICD_INTERFACE_VERSION;
        VkResult result = fp_negotiate_icd_version(pVersion);

        if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
            // ICD no longer supports the loader's latest interface version so
            // fail loading the ICD
            return false;
        }
    }

#if MIN_SUPPORTED_LOADER_ICD_INTERFACE_VERSION > 0
    if (*pVersion < MIN_SUPPORTED_LOADER_ICD_INTERFACE_VERSION) {
        // Loader no longer supports the ICD's latest interface version so fail
        // loading the ICD
        return false;
    }
#endif
    return true;
}

void loader_scanned_icd_clear(const struct loader_instance *inst, struct loader_icd_tramp_list *icd_tramp_list) {
    if (0 != icd_tramp_list->capacity) {
        for (uint32_t i = 0; i < icd_tramp_list->count; i++) {
            loader_platform_close_library(icd_tramp_list->scanned_list[i].handle);
            loader_instance_heap_free(inst, icd_tramp_list->scanned_list[i].lib_name);
        }
        loader_instance_heap_free(inst, icd_tramp_list->scanned_list);
        icd_tramp_list->capacity = 0;
        icd_tramp_list->count = 0;
        icd_tramp_list->scanned_list = NULL;
    }
}

static VkResult loader_scanned_icd_init(const struct loader_instance *inst, struct loader_icd_tramp_list *icd_tramp_list) {
    VkResult err = VK_SUCCESS;
    loader_scanned_icd_clear(inst, icd_tramp_list);
    icd_tramp_list->capacity = 8 * sizeof(struct loader_scanned_icd);
    icd_tramp_list->scanned_list = loader_instance_heap_alloc(inst, icd_tramp_list->capacity, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    if (NULL == icd_tramp_list->scanned_list) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "loader_scanned_icd_init: Realloc failed for layer list when attempting to add new layer");
        err = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return err;
}

static VkResult loader_scanned_icd_add(const struct loader_instance *inst, struct loader_icd_tramp_list *icd_tramp_list,
                                       const char *filename, uint32_t api_version) {
    loader_platform_dl_handle handle;
    PFN_vkCreateInstance fp_create_inst;
    PFN_vkEnumerateInstanceExtensionProperties fp_get_inst_ext_props;
    PFN_vkGetInstanceProcAddr fp_get_proc_addr;
    PFN_GetPhysicalDeviceProcAddr fp_get_phys_dev_proc_addr = NULL;
    PFN_vkNegotiateLoaderICDInterfaceVersion fp_negotiate_icd_version;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    PFN_vk_icdEnumerateAdapterPhysicalDevices fp_enum_dxgi_adapter_phys_devs = NULL;
#endif
    struct loader_scanned_icd *new_scanned_icd;
    uint32_t interface_vers;
    VkResult res = VK_SUCCESS;

    // TODO implement smarter opening/closing of libraries. For now this
    // function leaves libraries open and the scanned_icd_clear closes them
#if defined(__Fuchsia__)
    handle = loader_platform_open_driver(filename);
#else
    handle = loader_platform_open_library(filename);
#endif
    if (NULL == handle) {
        loader_log_load_library_error(inst, filename);
        res = VK_ERROR_INCOMPATIBLE_DRIVER;
        goto out;
    }

    // Get and settle on an ICD interface version
    fp_negotiate_icd_version = loader_platform_get_proc_address(handle, "vk_icdNegotiateLoaderICDInterfaceVersion");

    if (!loader_get_icd_interface_version(fp_negotiate_icd_version, &interface_vers)) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "loader_scanned_icd_add: ICD %s doesn't support interface version compatible with loader, skip this ICD.",
                   filename);
        goto out;
    }

    fp_get_proc_addr = loader_platform_get_proc_address(handle, "vk_icdGetInstanceProcAddr");
    if (NULL == fp_get_proc_addr) {
        assert(interface_vers == 0);
        // Use deprecated interface from version 0
        fp_get_proc_addr = loader_platform_get_proc_address(handle, "vkGetInstanceProcAddr");
        if (NULL == fp_get_proc_addr) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_scanned_icd_add: Attempt to retrieve either \'vkGetInstanceProcAddr\' or "
                       "\'vk_icdGetInstanceProcAddr\' from ICD %s failed.",
                       filename);
            goto out;
        } else {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "loader_scanned_icd_add: Using deprecated ICD interface of \'vkGetInstanceProcAddr\' instead of "
                       "\'vk_icdGetInstanceProcAddr\' for ICD %s",
                       filename);
        }
        fp_create_inst = loader_platform_get_proc_address(handle, "vkCreateInstance");
        if (NULL == fp_create_inst) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_scanned_icd_add:  Failed querying \'vkCreateInstance\' via dlsym/loadlibrary for ICD %s", filename);
            goto out;
        }
        fp_get_inst_ext_props = loader_platform_get_proc_address(handle, "vkEnumerateInstanceExtensionProperties");
        if (NULL == fp_get_inst_ext_props) {
            loader_log(
                inst, VULKAN_LOADER_ERROR_BIT, 0,
                "loader_scanned_icd_add: Could not get \'vkEnumerateInstanceExtensionProperties\' via dlsym/loadlibrary for ICD %s",
                filename);
            goto out;
        }
    } else {
        // Use newer interface version 1 or later
        if (interface_vers == 0) {
            interface_vers = 1;
        }

        fp_create_inst = (PFN_vkCreateInstance)fp_get_proc_addr(NULL, "vkCreateInstance");
        if (NULL == fp_create_inst) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_scanned_icd_add: Could not get \'vkCreateInstance\' via \'vk_icdGetInstanceProcAddr\' for ICD %s",
                       filename);
            goto out;
        }
        fp_get_inst_ext_props =
            (PFN_vkEnumerateInstanceExtensionProperties)fp_get_proc_addr(NULL, "vkEnumerateInstanceExtensionProperties");
        if (NULL == fp_get_inst_ext_props) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_scanned_icd_add: Could not get \'vkEnumerateInstanceExtensionProperties\' via "
                       "\'vk_icdGetInstanceProcAddr\' for ICD %s",
                       filename);
            goto out;
        }
        fp_get_phys_dev_proc_addr = loader_platform_get_proc_address(handle, "vk_icdGetPhysicalDeviceProcAddr");
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        if (interface_vers >= 6) {
            fp_enum_dxgi_adapter_phys_devs = loader_platform_get_proc_address(handle, "vk_icdEnumerateAdapterPhysicalDevices");
        }
#endif
    }

    // check for enough capacity
    if ((icd_tramp_list->count * sizeof(struct loader_scanned_icd)) >= icd_tramp_list->capacity) {
        void *new_ptr = loader_instance_heap_realloc(inst, icd_tramp_list->scanned_list, icd_tramp_list->capacity,
                                                     icd_tramp_list->capacity * 2, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
        if (NULL == new_ptr) {
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_scanned_icd_add: Realloc failed on icd library list for ICD %s",
                       filename);
            goto out;
        }
        icd_tramp_list->scanned_list = new_ptr;

        // double capacity
        icd_tramp_list->capacity *= 2;
    }

    uint32_t major_version = VK_API_VERSION_MAJOR(api_version);
    uint32_t minor_version = VK_API_VERSION_MINOR(api_version);
    if (interface_vers <= 4 && 1 == major_version && 0 < minor_version) {
        loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                   "loader_scanned_icd_add: Driver %s supports Vulkan %u.%u, but only supports loader interface version %u."
                   " Interface version 5 or newer required to support this version of Vulkan (Policy #LDP_DRIVER_7)",
                   filename, major_version, minor_version, interface_vers);
    }
    if (interface_vers >= 1) {
        if ((loader_platform_get_proc_address(handle, "vkEnumerateInstanceExtensionProperties") != NULL) ||
            (loader_platform_get_proc_address(handle, "vkEnumerateInstanceLayerProperties") != NULL) ||
            (loader_platform_get_proc_address(handle, "vkEnumerateInstanceVersion") != NULL) ||
            (loader_platform_get_proc_address(handle, "vkGetInstanceProcAddr") != NULL) ||
            (loader_platform_get_proc_address(handle, "vkCreateInstance") != NULL) ||
            (loader_platform_get_proc_address(handle, "vkGetDeviceProcAddr") != NULL) ||
            (loader_platform_get_proc_address(handle, "vkCreateDevice") != NULL)) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "loader_scanned_icd_add: Driver %s says it supports interface version %u but still exports core "
                       "entrypoints (Policy #LDP_DRIVER_6)",
                       filename, interface_vers);
        }
    }

    new_scanned_icd = &(icd_tramp_list->scanned_list[icd_tramp_list->count]);
    new_scanned_icd->handle = handle;
    new_scanned_icd->api_version = api_version;
    new_scanned_icd->GetInstanceProcAddr = fp_get_proc_addr;
    new_scanned_icd->GetPhysicalDeviceProcAddr = fp_get_phys_dev_proc_addr;
    new_scanned_icd->EnumerateInstanceExtensionProperties = fp_get_inst_ext_props;
    new_scanned_icd->CreateInstance = fp_create_inst;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    new_scanned_icd->EnumerateAdapterPhysicalDevices = fp_enum_dxgi_adapter_phys_devs;
#endif
    new_scanned_icd->interface_version = interface_vers;

    new_scanned_icd->lib_name = (char *)loader_instance_heap_alloc(inst, strlen(filename) + 1, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    if (NULL == new_scanned_icd->lib_name) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_scanned_icd_add: Out of memory can't add ICD %s", filename);
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }
    strcpy(new_scanned_icd->lib_name, filename);
    icd_tramp_list->count++;

out:

    return res;
}

void loader_initialize(void) {
    // initialize mutexes
    loader_platform_thread_create_mutex(&loader_lock);
    loader_platform_thread_create_mutex(&loader_json_lock);
    loader_platform_thread_create_mutex(&loader_preload_icd_lock);
    // initialize logging
    loader_debug_init();
#if defined(_WIN32)
    windows_initialization();
#endif

    loader_log(NULL, VULKAN_LOADER_INFO_BIT, 0, "Vulkan Loader Version %d.%d.%d", VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE),
               VK_API_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE), VK_API_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE));

#if defined(GIT_BRANCH_NAME) && defined(GIT_TAG_INFO)
#define LOADER_GIT_STRINGIFY(x) #x
#define LOADER_GIT_TOSTRING(x) LOADER_GIT_STRINGIFY(x)
    const char git_branch_name[] = LOADER_GIT_TOSTRING(GIT_BRANCH_NAME);
    const char git_tag_info[] = LOADER_GIT_TOSTRING(GIT_TAG_INFO);
    loader_log(NULL, VULKAN_LOADER_INFO_BIT, 0, "[Git - Tag: %s, Branch/Commit: %s]", git_tag_info, git_branch_name);
#endif
}

void loader_release() {
    // Guarantee release of the preloaded ICD libraries. This may have already been called in vkDestroyInstance.
    loader_unload_preloaded_icds();

    // release mutexes
    loader_platform_thread_delete_mutex(&loader_lock);
    loader_platform_thread_delete_mutex(&loader_json_lock);
    loader_platform_thread_delete_mutex(&loader_preload_icd_lock);
}

// Preload the ICD libraries that are likely to be needed so we don't repeatedly load/unload them later
void loader_preload_icds(void) {
    loader_platform_thread_lock_mutex(&loader_preload_icd_lock);

    // Already preloaded, skip loading again.
    if (scanned_icds.scanned_list != NULL) {
        loader_platform_thread_unlock_mutex(&loader_preload_icd_lock);
        return;
    }

    memset(&scanned_icds, 0, sizeof(scanned_icds));
    VkResult result = loader_icd_scan(NULL, &scanned_icds);
    if (result != VK_SUCCESS) {
        loader_scanned_icd_clear(NULL, &scanned_icds);
    }
    loader_platform_thread_unlock_mutex(&loader_preload_icd_lock);
}

// Release the ICD libraries that were preloaded
void loader_unload_preloaded_icds(void) {
    loader_platform_thread_lock_mutex(&loader_preload_icd_lock);
    loader_scanned_icd_clear(NULL, &scanned_icds);
    loader_platform_thread_unlock_mutex(&loader_preload_icd_lock);
}

#if !defined(_WIN32)
__attribute__((constructor)) void loader_init_library() { loader_initialize(); }

__attribute__((destructor)) void loader_free_library() { loader_release(); }
#endif

// Get next file or dirname given a string list or registry key path
//
// \returns
// A pointer to first char in the next path.
// The next path (or NULL) in the list is returned in next_path.
// Note: input string is modified in some cases. PASS IN A COPY!
char *loader_get_next_path(char *path) {
    uint32_t len;
    char *next;

    if (path == NULL) return NULL;
    next = strchr(path, PATH_SEPARATOR);
    if (next == NULL) {
        len = (uint32_t)strlen(path);
        next = path + len;
    } else {
        *next = '\0';
        next++;
    }

    return next;
}

// Given a path which is absolute or relative, expand the path if relative or
// leave the path unmodified if absolute. The base path to prepend to relative
// paths is given in rel_base.
//
// @return - A string in out_fullpath of the full absolute path
static void loader_expand_path(const char *path, const char *rel_base, size_t out_size, char *out_fullpath) {
    if (loader_platform_is_path_absolute(path)) {
        // do not prepend a base to an absolute path
        rel_base = "";
    }

    loader_platform_combine_path(out_fullpath, out_size, rel_base, path, NULL);
}

// Given a filename (file)  and a list of paths (dir), try to find an existing
// file in the paths.  If filename already is a path then no searching in the given paths.
//
// @return - A string in out_fullpath of either the full path or file.
static void loader_get_fullpath(const char *file, const char *dirs, size_t out_size, char *out_fullpath) {
    if (!loader_platform_is_path(file) && *dirs) {
        char *dirs_copy, *dir, *next_dir;

        dirs_copy = loader_stack_alloc(strlen(dirs) + 1);
        strcpy(dirs_copy, dirs);

        // find if file exists after prepending paths in given list
        for (dir = dirs_copy; *dir && (next_dir = loader_get_next_path(dir)); dir = next_dir) {
            loader_platform_combine_path(out_fullpath, out_size, dir, file, NULL);
            if (loader_platform_file_exists(out_fullpath)) {
                return;
            }
        }
    }

    (void)snprintf(out_fullpath, out_size, "%s", file);
}

// Read a JSON file into a buffer.
//
// @return -  A pointer to a cJSON object representing the JSON parse tree.
//            This returned buffer should be freed by caller.
static VkResult loader_get_json(const struct loader_instance *inst, const char *filename, cJSON **json) {
    FILE *file = NULL;
    char *json_buf = NULL;
    size_t len;
    VkResult res = VK_SUCCESS;

    if (NULL == json) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_get_json: Received invalid JSON file");
        res = VK_ERROR_INITIALIZATION_FAILED;
        goto out;
    }

    *json = NULL;

    file = fopen(filename, "rb");
    if (!file) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_get_json: Failed to open JSON file %s", filename);
        res = VK_ERROR_INITIALIZATION_FAILED;
        goto out;
    }
    // NOTE: We can't just use fseek(file, 0, SEEK_END) because that isn't guaranteed to be supported on all systems
    size_t fread_ret_count = 0;
    do {
        char buffer[256];
        fread_ret_count = fread(buffer, 1, 256, file);
    } while (fread_ret_count == 256 && !feof(file));
    len = ftell(file);
    fseek(file, 0, SEEK_SET);
    json_buf = (char *)loader_instance_heap_alloc(inst, len + 1, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (json_buf == NULL) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "loader_get_json: Failed to allocate space for JSON file %s buffer of length %d", filename, len);
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }
    if (fread(json_buf, sizeof(char), len, file) != len) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_get_json: Failed to read JSON file %s.", filename);
        res = VK_ERROR_INITIALIZATION_FAILED;
        goto out;
    }
    json_buf[len] = '\0';

    // Can't be a valid json if the string is of length zero
    if (len == 0) {
        res = VK_ERROR_INITIALIZATION_FAILED;
        goto out;
    }
    // Parse text from file
    *json = cJSON_Parse(inst, json_buf);
    if (*json == NULL) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "loader_get_json: Failed to parse JSON file %s, this is usually because something ran out of memory.", filename);
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }

out:
    if (NULL != json_buf) {
        loader_instance_heap_free(inst, json_buf);
    }
    if (NULL != file) {
        fclose(file);
    }

    return res;
}

// Verify that all component layers in a meta-layer are valid.
static bool verify_meta_layer_component_layers(const struct loader_instance *inst, struct loader_layer_properties *prop,
                                               struct loader_layer_list *instance_layers) {
    bool success = true;
    const uint32_t expected_major = VK_VERSION_MAJOR(prop->info.specVersion);
    const uint32_t expected_minor = VK_VERSION_MINOR(prop->info.specVersion);

    for (uint32_t comp_layer = 0; comp_layer < prop->num_component_layers; comp_layer++) {
        struct loader_layer_properties *comp_prop =
            loader_find_layer_property(prop->component_layer_names[comp_layer], instance_layers);
        if (comp_prop == NULL) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "verify_meta_layer_component_layers: Meta-layer %s can't find component layer %s at index %d."
                       "  Skipping this layer.",
                       prop->info.layerName, prop->component_layer_names[comp_layer], comp_layer);

            success = false;
            break;
        }

        // Check the version of each layer, they need to at least match MAJOR and MINOR
        uint32_t cur_major = VK_VERSION_MAJOR(comp_prop->info.specVersion);
        uint32_t cur_minor = VK_VERSION_MINOR(comp_prop->info.specVersion);
        if (cur_major != expected_major || cur_minor != expected_minor) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "verify_meta_layer_component_layers: Meta-layer uses API version %d.%d, but component "
                       "layer %d uses API version %d.%d.  Skipping this layer.",
                       expected_major, expected_minor, comp_layer, cur_major, cur_minor);

            success = false;
            break;
        }

        // Make sure the layer isn't using it's own name
        if (!strcmp(prop->info.layerName, prop->component_layer_names[comp_layer])) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "verify_meta_layer_component_layers: Meta-layer %s lists itself in its component layer "
                       "list at index %d.  Skipping this layer.",
                       prop->info.layerName, comp_layer);

            success = false;
            break;
        }
        if (comp_prop->type_flags & VK_LAYER_TYPE_FLAG_META_LAYER) {
            loader_log(inst, VULKAN_LOADER_INFO_BIT, 0,
                       "verify_meta_layer_component_layers: Adding meta-layer %s which also contains meta-layer %s",
                       prop->info.layerName, comp_prop->info.layerName);

            // Make sure if the layer is using a meta-layer in its component list that we also verify that.
            if (!verify_meta_layer_component_layers(inst, comp_prop, instance_layers)) {
                loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                           "Meta-layer %s component layer %s can not find all component layers."
                           "  Skipping this layer.",
                           prop->info.layerName, prop->component_layer_names[comp_layer]);
                success = false;
                break;
            }
        }

        // Add any instance and device extensions from component layers to this layer
        // list, so that anyone querying extensions will only need to look at the meta-layer
        for (uint32_t ext = 0; ext < comp_prop->instance_extension_list.count; ext++) {
            loader_log(inst, VULKAN_LOADER_DEBUG_BIT, 0, "Meta-layer %s component layer %s adding instance extension %s",
                       prop->info.layerName, prop->component_layer_names[comp_layer],
                       comp_prop->instance_extension_list.list[ext].extensionName);

            if (!has_vk_extension_property(&comp_prop->instance_extension_list.list[ext], &prop->instance_extension_list)) {
                loader_add_to_ext_list(inst, &prop->instance_extension_list, 1, &comp_prop->instance_extension_list.list[ext]);
            }
        }

        for (uint32_t ext = 0; ext < comp_prop->device_extension_list.count; ext++) {
            loader_log(inst, VULKAN_LOADER_DEBUG_BIT, 0, "Meta-layer %s component layer %s adding device extension %s",
                       prop->info.layerName, prop->component_layer_names[comp_layer],
                       comp_prop->device_extension_list.list[ext].props.extensionName);

            if (!has_vk_dev_ext_property(&comp_prop->device_extension_list.list[ext].props, &prop->device_extension_list)) {
                loader_add_to_dev_ext_list(inst, &prop->device_extension_list, &comp_prop->device_extension_list.list[ext].props, 0,
                                           NULL);
            }
        }
    }
    if (success) {
        loader_log(inst, VULKAN_LOADER_INFO_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                   "Meta-layer %s all %d component layers appear to be valid.", prop->info.layerName, prop->num_component_layers);

        // If layer logging is on, list the internals included in the meta-layer
        if ((loader_get_debug_level() & VULKAN_LOADER_LAYER_BIT) != 0) {
            for (uint32_t comp_layer = 0; comp_layer < prop->num_component_layers; comp_layer++) {
                loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "  [%d] %s", comp_layer, prop->component_layer_names[comp_layer]);
            }
        }
    }
    return success;
}

// Verify that all meta-layers in a layer list are valid.
static void verify_all_meta_layers(struct loader_instance *inst, struct loader_layer_list *instance_layers,
                                   bool *override_layer_present) {
    *override_layer_present = false;
    for (int32_t i = 0; i < (int32_t)instance_layers->count; i++) {
        struct loader_layer_properties *prop = &instance_layers->list[i];

        // If this is a meta-layer, make sure it is valid
        if ((prop->type_flags & VK_LAYER_TYPE_FLAG_META_LAYER) &&
            !verify_meta_layer_component_layers(inst, prop, instance_layers)) {
            loader_log(inst, VULKAN_LOADER_DEBUG_BIT, 0,
                       "Removing meta-layer %s from instance layer list since it appears invalid.", prop->info.layerName);

            loader_remove_layer_in_list(inst, instance_layers, i);
            i--;

        } else if (prop->is_override && loader_implicit_layer_is_enabled(inst, prop)) {
            *override_layer_present = true;
        }
    }
}

// If the current working directory matches any app_key_path of the layers, remove all other override layers.
// Otherwise if no matching app_key was found, remove all but the global override layer, which has no app_key_path.
static void remove_all_non_valid_override_layers(struct loader_instance *inst, struct loader_layer_list *instance_layers) {
    if (instance_layers == NULL) {
        return;
    }

    char cur_path[MAX_STRING_SIZE];
    char *ret = loader_platform_executable_path(cur_path, sizeof(cur_path));
    if (ret == NULL) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "remove_all_non_valid_override_layers: Failed to get executable path and name");
        return;
    }

    // Find out if there is an override layer with same the app_key_path as the path to the current executable.
    // If more than one is found, remove it and use the first layer
    // Remove any layers which aren't global and do not have the same app_key_path as the path to the current executable.
    bool found_active_override_layer = false;
    int global_layer_index = -1;
    for (uint32_t i = 0; i < instance_layers->count; i++) {
        struct loader_layer_properties *props = &instance_layers->list[i];
        if (strcmp(props->info.layerName, VK_OVERRIDE_LAYER_NAME) == 0) {
            if (props->num_app_key_paths > 0) {  // not the global layer
                for (uint32_t j = 0; j < props->num_app_key_paths; j++) {
                    if (strcmp(props->app_key_paths[j], cur_path) == 0) {
                        if (!found_active_override_layer) {
                            found_active_override_layer = true;
                        } else {
                            loader_log(
                                inst, VULKAN_LOADER_WARN_BIT, 0,
                                "remove_all_non_valid_override_layers: Multiple override layers where the samepath in app_keys "
                                "was found. Using the first layer found");

                            // Remove duplicate active override layers that have the same app_key_path
                            loader_remove_layer_in_list(inst, instance_layers, i);
                            i--;
                        }
                    }
                }
                if (!found_active_override_layer) {
                    // Remove non-global override layers that don't have an app_key that matches cur_path
                    loader_remove_layer_in_list(inst, instance_layers, i);
                    i--;
                }
            } else {
                if (global_layer_index == -1) {
                    global_layer_index = i;
                } else {
                    loader_log(
                        inst, VULKAN_LOADER_WARN_BIT, 0,
                        "remove_all_non_valid_override_layers: Multiple global override layers found. Using the first global "
                        "layer found");
                    loader_remove_layer_in_list(inst, instance_layers, i);
                    i--;
                }
            }
        }
    }
    // Remove global layer if layer with same the app_key_path as the path to the current executable is found
    if (found_active_override_layer && global_layer_index >= 0) {
        loader_remove_layer_in_list(inst, instance_layers, global_layer_index);
    }
    // Should be at most 1 override layer in the list now.
    if (found_active_override_layer) {
        loader_log(inst, VULKAN_LOADER_INFO_BIT | VULKAN_LOADER_LAYER_BIT, 0, "Using the override layer for app key %s", cur_path);
    } else if (global_layer_index >= 0) {
        loader_log(inst, VULKAN_LOADER_INFO_BIT | VULKAN_LOADER_LAYER_BIT, 0, "Using the global override layer");
    }
}

// This structure is used to store the json file version
// in a more manageable way.
typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} layer_json_version;

static inline bool layer_json_supports_pre_instance_tag(const layer_json_version *layer_json) {
    // Supported versions started in 1.1.2, so anything newer
    return layer_json->major > 1 || layer_json->minor > 1 || (layer_json->minor == 1 && layer_json->patch > 1);
}

static VkResult loader_read_layer_json(const struct loader_instance *inst, struct loader_layer_list *layer_instance_list,
                                       cJSON *layer_node, layer_json_version version, cJSON *item, bool is_implicit,
                                       char *filename) {
    char *temp;
    char *name, *type, *library_path_str, *api_version;
    char *implementation_version, *description;
    cJSON *ext_item;
    cJSON *library_path;
    cJSON *component_layers;
    cJSON *override_paths;
    cJSON *blacklisted_layers;
    cJSON *disable_environment = NULL;
    VkExtensionProperties ext_prop;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    struct loader_layer_properties *props = NULL;
    uint32_t props_index = 0;
    int i, j;

// The following are required in the "layer" object:
// (required) "name"
// (required) "type"
// (required) "library_path"
// (required) "api_version"
// (required) "implementation_version"
// (required) "description"
// (required for implicit layers) "disable_environment"
#define GET_JSON_OBJECT(node, var)                                         \
    {                                                                      \
        var = cJSON_GetObjectItem(node, #var);                             \
        if (var == NULL) {                                                 \
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,                    \
                       "Didn't find required layer object %s in manifest " \
                       "JSON file, skipping this layer",                   \
                       #var);                                              \
            goto out;                                                      \
        }                                                                  \
    }
#define GET_JSON_ITEM(inst, node, var)                                         \
    {                                                                          \
        item = cJSON_GetObjectItem(node, #var);                                \
        if (item == NULL) {                                                    \
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,                        \
                       "Didn't find required layer value %s in manifest JSON " \
                       "file, skipping this layer",                            \
                       #var);                                                  \
            goto out;                                                          \
        }                                                                      \
        temp = cJSON_Print(inst, item);                                        \
        if (temp == NULL) {                                                    \
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,                        \
                       "Problem accessing layer value %s in manifest JSON "    \
                       "file, skipping this layer",                            \
                       #var);                                                  \
            result = VK_ERROR_OUT_OF_HOST_MEMORY;                              \
            goto out;                                                          \
        }                                                                      \
        temp[strlen(temp) - 1] = '\0';                                         \
        var = loader_stack_alloc(strlen(temp) + 1);                            \
        strcpy(var, &temp[1]);                                                 \
        cJSON_Free(inst, temp);                                                \
    }
    GET_JSON_ITEM(inst, layer_node, name)
    GET_JSON_ITEM(inst, layer_node, type)
    GET_JSON_ITEM(inst, layer_node, api_version)
    GET_JSON_ITEM(inst, layer_node, implementation_version)
    GET_JSON_ITEM(inst, layer_node, description)

    // Add list entry
    if (!strcmp(type, "DEVICE")) {
        loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_LAYER_BIT, 0, "Device layers are deprecated. Skipping this layer");
        goto out;
    }

    // Allow either GLOBAL or INSTANCE type interchangeably to handle
    // layers that must work with older loaders
    if (!strcmp(type, "INSTANCE") || !strcmp(type, "GLOBAL")) {
        if (layer_instance_list == NULL) {
            goto out;
        }
        props = loader_get_next_layer_property_slot(inst, layer_instance_list);
        if (NULL == props) {
            // Error already triggered in loader_get_next_layer_property_slot.
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto out;
        }
        props_index = layer_instance_list->count - 1;
        props->type_flags = VK_LAYER_TYPE_FLAG_INSTANCE_LAYER;
        if (!is_implicit) {
            props->type_flags |= VK_LAYER_TYPE_FLAG_EXPLICIT_LAYER;
        }
    } else {
        goto out;
    }

    // Expiration date for override layer.  Field starte with JSON file 1.1.2 and
    // is completely optional.  So, no check put in place.
    if (!strcmp(name, VK_OVERRIDE_LAYER_NAME)) {
        cJSON *expiration;

        if (version.major == 0 || (version.minor == 1 && version.patch < 2) || version.minor == 0) {
            loader_log(
                inst, VULKAN_LOADER_WARN_BIT, 0,
                "Override layer expiration date not added until version 1.1.2.  Please update JSON file version appropriately.");
        }

        props->is_override = true;
        expiration = cJSON_GetObjectItem(layer_node, "expiration_date");
        if (NULL != expiration) {
            char date_copy[32];
            uint8_t cur_item = 0;

            // Get the string for the current item
            temp = cJSON_Print(inst, expiration);
            if (temp == NULL) {
                loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                           "Problem accessing layer value 'expiration_date' in manifest JSON file, skipping this layer");
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto out;
            }
            temp[strlen(temp) - 1] = '\0';
            strcpy(date_copy, &temp[1]);
            cJSON_Free(inst, temp);

            if (strlen(date_copy) == 16) {
                char *cur_start = &date_copy[0];
                char *next_dash = strchr(date_copy, '-');
                if (NULL != next_dash) {
                    while (cur_item < 5 && strlen(cur_start)) {
                        if (next_dash != NULL) {
                            *next_dash = '\0';
                        }
                        switch (cur_item) {
                            case 0:  // Year
                                props->expiration.year = atoi(cur_start);
                                break;
                            case 1:  // Month
                                props->expiration.month = atoi(cur_start);
                                break;
                            case 2:  // Day
                                props->expiration.day = atoi(cur_start);
                                break;
                            case 3:  // Hour
                                props->expiration.hour = atoi(cur_start);
                                break;
                            case 4:  // Minute
                                props->expiration.minute = atoi(cur_start);
                                props->has_expiration = true;
                                break;
                            default:  // Ignore
                                break;
                        }
                        if (next_dash != NULL) {
                            cur_start = next_dash + 1;
                            next_dash = strchr(cur_start, '-');
                        }
                        cur_item++;
                    }
                }
            }
        }
    }

    // Library path no longer required unless component_layers is also not defined
    library_path = cJSON_GetObjectItem(layer_node, "library_path");
    component_layers = cJSON_GetObjectItem(layer_node, "component_layers");
    if (NULL != library_path) {
        if (NULL != component_layers) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "Indicating meta-layer-specific component_layers, but also defining layer library path.  Both are not "
                       "compatible, so skipping this layer");
            goto out;
        }
        props->num_component_layers = 0;
        props->component_layer_names = NULL;

        temp = cJSON_Print(inst, library_path);
        if (NULL == temp) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "Problem accessing layer value library_path in manifest JSON file, skipping this layer");
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto out;
        }
        temp[strlen(temp) - 1] = '\0';
        library_path_str = loader_stack_alloc(strlen(temp) + 1);
        strcpy(library_path_str, &temp[1]);
        cJSON_Free(inst, temp);

        strncpy(props->manifest_file_name, filename, MAX_STRING_SIZE);
        char *fullpath = props->lib_name;
        char *rel_base;
        if (NULL != library_path_str) {
            if (loader_platform_is_path(library_path_str)) {
                // A relative or absolute path
                char *name_copy = loader_stack_alloc(strlen(filename) + 1);
                strcpy(name_copy, filename);
                rel_base = loader_platform_dirname(name_copy);
                loader_expand_path(library_path_str, rel_base, MAX_STRING_SIZE, fullpath);
            } else {
// A filename which is assumed in a system directory
#if defined(DEFAULT_VK_LAYERS_PATH)
                loader_get_fullpath(library_path_str, DEFAULT_VK_LAYERS_PATH, MAX_STRING_SIZE, fullpath);
#else
                loader_get_fullpath(library_path_str, "", MAX_STRING_SIZE, fullpath);
#endif
            }
        }
    } else if (NULL != component_layers) {
        if (version.major == 0 || (version.minor == 1 && version.patch < 1) || (version.minor == 0)) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "Indicating meta-layer-specific component_layers, but using older JSON file version.");
        }
        int count = cJSON_GetArraySize(component_layers);
        props->num_component_layers = count;

        // Allocate buffer for layer names
        props->component_layer_names =
            loader_instance_heap_alloc(inst, sizeof(char[MAX_STRING_SIZE]) * count, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
        if (NULL == props->component_layer_names && count > 0) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto out;
        }

        // Copy the component layers into the array
        for (i = 0; i < count; i++) {
            cJSON *comp_layer = cJSON_GetArrayItem(component_layers, i);
            if (NULL != comp_layer) {
                temp = cJSON_Print(inst, comp_layer);
                if (NULL == temp) {
                    result = VK_ERROR_OUT_OF_HOST_MEMORY;
                    goto out;
                }
                temp[strlen(temp) - 1] = '\0';
                strncpy(props->component_layer_names[i], temp + 1, MAX_STRING_SIZE - 1);
                props->component_layer_names[i][MAX_STRING_SIZE - 1] = '\0';
                cJSON_Free(inst, temp);
            }
        }

        // This is now, officially, a meta-layer
        props->type_flags |= VK_LAYER_TYPE_FLAG_META_LAYER;
        loader_log(inst, VULKAN_LOADER_INFO_BIT | VULKAN_LOADER_LAYER_BIT, 0, "Encountered meta-layer %s", name);

        // Make sure we set up other things so we head down the correct branches below
        library_path_str = NULL;
    } else {
        loader_log(
            inst, VULKAN_LOADER_WARN_BIT, 0,
            "Layer missing both library_path and component_layers fields.  One or the other MUST be defined.  Skipping this layer");
        goto out;
    }

    props->num_blacklist_layers = 0;
    props->blacklist_layer_names = NULL;
    blacklisted_layers = cJSON_GetObjectItem(layer_node, "blacklisted_layers");
    if (blacklisted_layers != NULL) {
        if (strcmp(name, VK_OVERRIDE_LAYER_NAME)) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "Layer %s contains a blacklist, but a blacklist can only be provided by the override metalayer. This "
                       "blacklist will be ignored.",
                       name);
        } else {
            props->num_blacklist_layers = cJSON_GetArraySize(blacklisted_layers);
            if (props->num_blacklist_layers > 0) {
                // Allocate the blacklist array
                props->blacklist_layer_names = loader_instance_heap_alloc(
                    inst, sizeof(char[MAX_STRING_SIZE]) * props->num_blacklist_layers, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
                if (props->blacklist_layer_names == NULL && props->num_blacklist_layers > 0) {
                    result = VK_ERROR_OUT_OF_HOST_MEMORY;
                    goto out;
                }

                // Copy the blacklisted layers into the array
                for (i = 0; i < (int)props->num_blacklist_layers; ++i) {
                    cJSON *black_layer = cJSON_GetArrayItem(blacklisted_layers, i);
                    if (black_layer == NULL) {
                        continue;
                    }
                    temp = cJSON_Print(inst, black_layer);
                    if (temp == NULL) {
                        result = VK_ERROR_OUT_OF_HOST_MEMORY;
                        goto out;
                    }
                    temp[strlen(temp) - 1] = '\0';
                    strncpy(props->blacklist_layer_names[i], temp + 1, MAX_STRING_SIZE - 1);
                    props->blacklist_layer_names[i][MAX_STRING_SIZE - 1] = '\0';
                    cJSON_Free(inst, temp);
                }
            }
        }
    }

    override_paths = cJSON_GetObjectItem(layer_node, "override_paths");
    if (NULL != override_paths) {
        if (version.major == 0 || (version.minor == 1 && version.patch) < 1 || version.minor == 0) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "Indicating meta-layer-specific override paths, but using older JSON file version.");
        }
        int count = cJSON_GetArraySize(override_paths);
        props->num_override_paths = count;
        if (count > 0) {
            // Allocate buffer for override paths
            props->override_paths =
                loader_instance_heap_alloc(inst, sizeof(char[MAX_STRING_SIZE]) * count, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
            if (NULL == props->override_paths && count > 0) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto out;
            }

            // Copy the override paths into the array
            for (i = 0; i < count; i++) {
                cJSON *override_path = cJSON_GetArrayItem(override_paths, i);
                if (NULL != override_path) {
                    temp = cJSON_Print(inst, override_path);
                    if (NULL == temp) {
                        result = VK_ERROR_OUT_OF_HOST_MEMORY;
                        goto out;
                    }
                    temp[strlen(temp) - 1] = '\0';
                    strncpy(props->override_paths[i], temp + 1, MAX_STRING_SIZE - 1);
                    props->override_paths[i][MAX_STRING_SIZE - 1] = '\0';
                    cJSON_Free(inst, temp);
                }
            }
        }
    }

    if (is_implicit) {
        GET_JSON_OBJECT(layer_node, disable_environment)
    }
#undef GET_JSON_ITEM
#undef GET_JSON_OBJECT

    strncpy(props->info.layerName, name, sizeof(props->info.layerName));
    props->info.layerName[sizeof(props->info.layerName) - 1] = '\0';
    if (0 != strncmp(props->info.layerName, "VK_LAYER_", 9)) {
        loader_log(inst, VULKAN_LOADER_WARN_BIT, 0, "Layer name %s does not conform to naming standard (Policy #LLP_LAYER_3)",
                   props->info.layerName);
    }
    props->info.specVersion = loader_make_version(api_version);
    props->info.implementationVersion = atoi(implementation_version);
    strncpy((char *)props->info.description, description, sizeof(props->info.description));
    props->info.description[sizeof(props->info.description) - 1] = '\0';
    if (is_implicit) {
        if (!disable_environment || !disable_environment->child) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "Didn't find required layer child value disable_environment in manifest JSON file, skipping this layer "
                       "(Policy #LLP_LAYER_9)");
            goto out;
        }
        strncpy(props->disable_env_var.name, disable_environment->child->string, sizeof(props->disable_env_var.name));
        props->disable_env_var.name[sizeof(props->disable_env_var.name) - 1] = '\0';
        strncpy(props->disable_env_var.value, disable_environment->child->valuestring, sizeof(props->disable_env_var.value));
        props->disable_env_var.value[sizeof(props->disable_env_var.value) - 1] = '\0';
    }

// Now get all optional items and objects and put in list:
// functions
// instance_extensions
// device_extensions
// enable_environment (implicit layers only)
#define GET_JSON_OBJECT(node, var) \
    { var = cJSON_GetObjectItem(node, #var); }
#define GET_JSON_ITEM(inst, node, var)                      \
    {                                                       \
        item = cJSON_GetObjectItem(node, #var);             \
        if (item != NULL) {                                 \
            temp = cJSON_Print(inst, item);                 \
            if (temp != NULL) {                             \
                temp[strlen(temp) - 1] = '\0';              \
                var = loader_stack_alloc(strlen(temp) + 1); \
                strcpy(var, &temp[1]);                      \
                cJSON_Free(inst, temp);                     \
            } else {                                        \
                result = VK_ERROR_OUT_OF_HOST_MEMORY;       \
                goto out;                                   \
            }                                               \
        }                                                   \
    }

    cJSON *instance_extensions, *device_extensions, *functions, *enable_environment;
    cJSON *entrypoints = NULL;
    char *vkGetInstanceProcAddr = NULL;
    char *vkGetDeviceProcAddr = NULL;
    char *vkNegotiateLoaderLayerInterfaceVersion = NULL;
    char *spec_version = NULL;
    char **entry_array = NULL;
    cJSON *app_keys = NULL;

    // Layer interface functions
    //    vkGetInstanceProcAddr
    //    vkGetDeviceProcAddr
    //    vkNegotiateLoaderLayerInterfaceVersion (starting with JSON file 1.1.0)
    GET_JSON_OBJECT(layer_node, functions)
    if (functions != NULL) {
        if (version.major > 1 || version.minor >= 1) {
            GET_JSON_ITEM(inst, functions, vkNegotiateLoaderLayerInterfaceVersion)
            if (vkNegotiateLoaderLayerInterfaceVersion != NULL)
                strncpy(props->functions.str_negotiate_interface, vkNegotiateLoaderLayerInterfaceVersion,
                        sizeof(props->functions.str_negotiate_interface));
            props->functions.str_negotiate_interface[sizeof(props->functions.str_negotiate_interface) - 1] = '\0';
        } else {
            props->functions.str_negotiate_interface[0] = '\0';
        }
        GET_JSON_ITEM(inst, functions, vkGetInstanceProcAddr)
        GET_JSON_ITEM(inst, functions, vkGetDeviceProcAddr)
        if (vkGetInstanceProcAddr != NULL) {
            strncpy(props->functions.str_gipa, vkGetInstanceProcAddr, sizeof(props->functions.str_gipa));
            if (version.major > 1 || version.minor >= 1) {
                loader_log(inst, VULKAN_LOADER_INFO_BIT, 0,
                           "Layer \"%s\" using deprecated \'vkGetInstanceProcAddr\' tag which was deprecated starting with JSON "
                           "file version 1.1.0. The new vkNegotiateLoaderLayerInterfaceVersion function is preferred, though for "
                           "compatibility reasons it may be desirable to continue using the deprecated tag.",
                           name);
            }
        }
        props->functions.str_gipa[sizeof(props->functions.str_gipa) - 1] = '\0';
        if (vkGetDeviceProcAddr != NULL) {
            strncpy(props->functions.str_gdpa, vkGetDeviceProcAddr, sizeof(props->functions.str_gdpa));
            if (version.major > 1 || version.minor >= 1) {
                loader_log(inst, VULKAN_LOADER_INFO_BIT, 0,
                           "Layer \"%s\" using deprecated \'vkGetDeviceProcAddr\' tag which was deprecated starting with JSON "
                           "file version 1.1.0. The new vkNegotiateLoaderLayerInterfaceVersion function is preferred, though for "
                           "compatibility reasons it may be desirable to continue using the deprecated tag.",
                           name);
            }
        }
        props->functions.str_gdpa[sizeof(props->functions.str_gdpa) - 1] = '\0';
    }

    // instance_extensions
    //   array of {
    //     name
    //     spec_version
    //   }
    GET_JSON_OBJECT(layer_node, instance_extensions)
    if (instance_extensions != NULL) {
        int count = cJSON_GetArraySize(instance_extensions);
        for (i = 0; i < count; i++) {
            ext_item = cJSON_GetArrayItem(instance_extensions, i);
            GET_JSON_ITEM(inst, ext_item, name)
            if (name != NULL) {
                strncpy(ext_prop.extensionName, name, sizeof(ext_prop.extensionName));
                ext_prop.extensionName[sizeof(ext_prop.extensionName) - 1] = '\0';
            }
            GET_JSON_ITEM(inst, ext_item, spec_version)
            if (NULL != spec_version) {
                ext_prop.specVersion = atoi(spec_version);
            } else {
                ext_prop.specVersion = 0;
            }
            bool ext_unsupported = wsi_unsupported_instance_extension(&ext_prop);
            if (!ext_unsupported) {
                loader_add_to_ext_list(inst, &props->instance_extension_list, 1, &ext_prop);
            }
        }
    }

    // device_extensions
    //   array of {
    //     name
    //     spec_version
    //     entrypoints
    //   }
    GET_JSON_OBJECT(layer_node, device_extensions)
    if (device_extensions != NULL) {
        int count = cJSON_GetArraySize(device_extensions);
        for (i = 0; i < count; i++) {
            ext_item = cJSON_GetArrayItem(device_extensions, i);
            GET_JSON_ITEM(inst, ext_item, name)
            GET_JSON_ITEM(inst, ext_item, spec_version)
            if (name != NULL) {
                strncpy(ext_prop.extensionName, name, sizeof(ext_prop.extensionName));
                ext_prop.extensionName[sizeof(ext_prop.extensionName) - 1] = '\0';
            }
            if (NULL != spec_version) {
                ext_prop.specVersion = atoi(spec_version);
            } else {
                ext_prop.specVersion = 0;
            }
            // entrypoints = cJSON_GetObjectItem(ext_item, "entrypoints");
            GET_JSON_OBJECT(ext_item, entrypoints)
            int entry_count;
            if (entrypoints == NULL) {
                loader_add_to_dev_ext_list(inst, &props->device_extension_list, &ext_prop, 0, NULL);
                continue;
            }
            entry_count = cJSON_GetArraySize(entrypoints);
            if (entry_count) {
                entry_array = (char **)loader_stack_alloc(sizeof(char *) * entry_count);
            }
            for (j = 0; j < entry_count; j++) {
                ext_item = cJSON_GetArrayItem(entrypoints, j);
                if (ext_item != NULL) {
                    temp = cJSON_Print(inst, ext_item);
                    if (NULL == temp) {
                        entry_array[j] = NULL;
                        result = VK_ERROR_OUT_OF_HOST_MEMORY;
                        goto out;
                    }
                    temp[strlen(temp) - 1] = '\0';
                    entry_array[j] = loader_stack_alloc(strlen(temp) + 1);
                    strcpy(entry_array[j], &temp[1]);
                    cJSON_Free(inst, temp);
                }
            }
            loader_add_to_dev_ext_list(inst, &props->device_extension_list, &ext_prop, entry_count, entry_array);
        }
    }
    if (is_implicit) {
        GET_JSON_OBJECT(layer_node, enable_environment)

        // enable_environment is optional
        if (enable_environment) {
            strncpy(props->enable_env_var.name, enable_environment->child->string, sizeof(props->enable_env_var.name));
            props->enable_env_var.name[sizeof(props->enable_env_var.name) - 1] = '\0';
            strncpy(props->enable_env_var.value, enable_environment->child->valuestring, sizeof(props->enable_env_var.value));
            props->enable_env_var.value[sizeof(props->enable_env_var.value) - 1] = '\0';
        }
    }

    // Read in the pre-instance stuff
    cJSON *pre_instance = cJSON_GetObjectItem(layer_node, "pre_instance_functions");
    if (pre_instance) {
        if (!layer_json_supports_pre_instance_tag(&version)) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "Found pre_instance_functions section in layer from \"%s\". This section is only valid in manifest version "
                       "1.1.2 or later. The section will be ignored",
                       filename);
        } else if (!is_implicit) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "Found pre_instance_functions section in explicit layer from \"%s\". This section is only valid in implicit "
                       "layers. The section will be ignored",
                       filename);
        } else {
            cJSON *inst_ext_json = cJSON_GetObjectItem(pre_instance, "vkEnumerateInstanceExtensionProperties");
            if (inst_ext_json) {
                char *inst_ext_name = cJSON_Print(inst, inst_ext_json);
                if (inst_ext_name == NULL) {
                    result = VK_ERROR_OUT_OF_HOST_MEMORY;
                    goto out;
                }
                size_t len = strlen(inst_ext_name) >= MAX_STRING_SIZE ? MAX_STRING_SIZE - 3 : strlen(inst_ext_name) - 2;
                strncpy(props->pre_instance_functions.enumerate_instance_extension_properties, inst_ext_name + 1, len);
                props->pre_instance_functions.enumerate_instance_extension_properties[len] = '\0';
                cJSON_Free(inst, inst_ext_name);
            }

            cJSON *inst_layer_json = cJSON_GetObjectItem(pre_instance, "vkEnumerateInstanceLayerProperties");
            if (inst_layer_json) {
                char *inst_layer_name = cJSON_Print(inst, inst_layer_json);
                if (inst_layer_name == NULL) {
                    result = VK_ERROR_OUT_OF_HOST_MEMORY;
                    goto out;
                }
                size_t len = strlen(inst_layer_name) >= MAX_STRING_SIZE ? MAX_STRING_SIZE - 3 : strlen(inst_layer_name) - 2;
                strncpy(props->pre_instance_functions.enumerate_instance_layer_properties, inst_layer_name + 1, len);
                props->pre_instance_functions.enumerate_instance_layer_properties[len] = '\0';
                cJSON_Free(inst, inst_layer_name);
            }

            cJSON *inst_version_json = cJSON_GetObjectItem(pre_instance, "vkEnumerateInstanceVersion");
            if (inst_version_json) {
                char *inst_version_name = cJSON_Print(inst, inst_version_json);
                if (inst_version_json) {
                    result = VK_ERROR_OUT_OF_HOST_MEMORY;
                    goto out;
                }
                size_t len = strlen(inst_version_name) >= MAX_STRING_SIZE ? MAX_STRING_SIZE - 3 : strlen(inst_version_name) - 2;
                strncpy(props->pre_instance_functions.enumerate_instance_version, inst_version_name + 1, len);
                props->pre_instance_functions.enumerate_instance_version[len] = '\0';
                cJSON_Free(inst, inst_version_name);
            }
        }
    }

    props->num_app_key_paths = 0;
    props->app_key_paths = NULL;
    app_keys = cJSON_GetObjectItem(layer_node, "app_keys");
    if (app_keys != NULL) {
        if (strcmp(name, VK_OVERRIDE_LAYER_NAME)) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT, 0,
                       "Layer %s contains app_keys, but any app_keys can only be provided by the override metalayer. "
                       "These will be ignored.",
                       name);
        } else {
            props->num_app_key_paths = cJSON_GetArraySize(app_keys);

            // Allocate the blacklist array
            props->app_key_paths = loader_instance_heap_alloc(inst, sizeof(char[MAX_STRING_SIZE]) * props->num_app_key_paths,
                                                              VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
            if (props->app_key_paths == NULL) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto out;
            }

            // Copy the app_key_paths into the array
            for (i = 0; i < (int)props->num_app_key_paths; ++i) {
                cJSON *app_key_path = cJSON_GetArrayItem(app_keys, i);
                if (app_key_path == NULL) {
                    continue;
                }
                temp = cJSON_Print(inst, app_key_path);
                if (temp == NULL) {
                    result = VK_ERROR_OUT_OF_HOST_MEMORY;
                    goto out;
                }
                temp[strlen(temp) - 1] = '\0';
                strncpy(props->app_key_paths[i], temp + 1, MAX_STRING_SIZE - 1);
                props->app_key_paths[i][MAX_STRING_SIZE - 1] = '\0';
                cJSON_Free(inst, temp);
            }
        }
    }

    result = VK_SUCCESS;

out:
#undef GET_JSON_ITEM
#undef GET_JSON_OBJECT

    if (VK_SUCCESS != result && NULL != props) {
        // Make sure to free anything that was allocated
        loader_remove_layer_in_list(inst, layer_instance_list, props_index);
    }

    return result;
}

static inline bool is_valid_layer_json_version(const layer_json_version *layer_json) {
    // Supported versions are: 1.0.0, 1.0.1, 1.1.0 - 1.1.2, and 1.2.0 - 1.2.1.
    if ((layer_json->major == 1 && layer_json->minor == 2 && layer_json->patch < 2) ||
        (layer_json->major == 1 && layer_json->minor == 1 && layer_json->patch < 3) ||
        (layer_json->major == 1 && layer_json->minor == 0 && layer_json->patch < 2)) {
        return true;
    }
    return false;
}

static inline bool layer_json_supports_multiple_layers(const layer_json_version *layer_json) {
    // Supported versions started in 1.0.1, so anything newer
    if ((layer_json->major > 1 || layer_json->minor > 0 || layer_json->patch > 1)) {
        return true;
    }
    return false;
}

// Given a cJSON struct (json) of the top level JSON object from layer manifest
// file, add entry to the layer_list. Fill out the layer_properties in this list
// entry from the input cJSON object.
//
// \returns
// void
// layer_list has a new entry and initialized accordingly.
// If the json input object does not have all the required fields no entry
// is added to the list.
static VkResult loader_add_layer_properties(const struct loader_instance *inst, struct loader_layer_list *layer_instance_list,
                                            cJSON *json, bool is_implicit, char *filename) {
    // The following Fields in layer manifest file that are required:
    //   - "file_format_version"
    //   - If more than one "layer" object are used, then the "layers" array is
    //     required
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    cJSON *item, *layers_node, *layer_node;
    layer_json_version json_version = {0, 0, 0};
    char *vers_tok;
    // Make sure sure the top level json value is an object
    if (!json || json->type != 6) {
        goto out;
    }
    item = cJSON_GetObjectItem(json, "file_format_version");
    if (item == NULL) {
        goto out;
    }
    char *file_vers = cJSON_PrintUnformatted(inst, item);
    if (NULL == file_vers) {
        goto out;
    }
    loader_log(inst, VULKAN_LOADER_INFO_BIT, 0, "Found manifest file %s (file version %s)", filename, file_vers);
    // Get the major/minor/and patch as integers for easier comparison
    vers_tok = strtok(file_vers, ".\"\n\r");
    if (NULL != vers_tok) {
        json_version.major = (uint16_t)atoi(vers_tok);
        vers_tok = strtok(NULL, ".\"\n\r");
        if (NULL != vers_tok) {
            json_version.minor = (uint16_t)atoi(vers_tok);
            vers_tok = strtok(NULL, ".\"\n\r");
            if (NULL != vers_tok) {
                json_version.patch = (uint16_t)atoi(vers_tok);
            }
        }
    }

    if (!is_valid_layer_json_version(&json_version)) {
        loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                   "loader_add_layer_properties: %s invalid layer manifest file version %d.%d.%d.  May cause errors.", filename,
                   json_version.major, json_version.minor, json_version.patch);
    }
    cJSON_Free(inst, file_vers);

    // If "layers" is present, read in the array of layer objects
    layers_node = cJSON_GetObjectItem(json, "layers");
    if (layers_node != NULL) {
        int numItems = cJSON_GetArraySize(layers_node);
        if (!layer_json_supports_multiple_layers(&json_version)) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                       "loader_add_layer_properties: \'layers\' tag not supported until file version 1.0.1, but %s is reporting "
                       "version %s",
                       filename, file_vers);
        }
        for (int curLayer = 0; curLayer < numItems; curLayer++) {
            layer_node = cJSON_GetArrayItem(layers_node, curLayer);
            if (layer_node == NULL) {
                loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                           "loader_add_layer_properties: Can not find 'layers' array element %d object in manifest JSON file %s.  "
                           "Skipping this file",
                           curLayer, filename);
                goto out;
            }
            result = loader_read_layer_json(inst, layer_instance_list, layer_node, json_version, item, is_implicit, filename);
        }
    } else {
        // Otherwise, try to read in individual layers
        layer_node = cJSON_GetObjectItem(json, "layer");
        if (layer_node == NULL) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                       "loader_add_layer_properties: Can not find 'layer' object in manifest JSON file %s.  Skipping this file.",
                       filename);
            goto out;
        }
        // Loop through all "layer" objects in the file to get a count of them
        // first.
        uint16_t layer_count = 0;
        cJSON *tempNode = layer_node;
        do {
            tempNode = tempNode->next;
            layer_count++;
        } while (tempNode != NULL);

        // Throw a warning if we encounter multiple "layer" objects in file
        // versions newer than 1.0.0.  Having multiple objects with the same
        // name at the same level is actually a JSON standard violation.
        if (layer_count > 1 && layer_json_supports_multiple_layers(&json_version)) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                       "loader_add_layer_properties: Multiple 'layer' nodes are deprecated starting in file version \"1.0.1\".  "
                       "Please use 'layers' : [] array instead in %s.",
                       filename);
        } else {
            do {
                result = loader_read_layer_json(inst, layer_instance_list, layer_node, json_version, item, is_implicit, filename);
                layer_node = layer_node->next;
            } while (layer_node != NULL);
        }
    }

out:

    return result;
}

static inline size_t determine_data_file_path_size(const char *cur_path, size_t relative_path_size) {
    size_t path_size = 0;

    if (NULL != cur_path) {
        // For each folder in cur_path, (detected by finding additional
        // path separators in the string) we need to add the relative path on
        // the end.  Plus, leave an additional two slots on the end to add an
        // additional directory slash and path separator if needed
        path_size += strlen(cur_path) + relative_path_size + 2;
        for (const char *x = cur_path; *x; ++x) {
            if (*x == PATH_SEPARATOR) {
                path_size += relative_path_size + 2;
            }
        }
    }

    return path_size;
}

static inline void copy_data_file_path(const char *cur_path, const char *relative_path, size_t relative_path_size,
                                       char **output_path) {
    if (NULL != cur_path) {
        uint32_t start = 0;
        uint32_t stop = 0;
        char *cur_write = *output_path;

        while (cur_path[start] != '\0') {
            while (cur_path[start] == PATH_SEPARATOR) {
                start++;
            }
            stop = start;
            while (cur_path[stop] != PATH_SEPARATOR && cur_path[stop] != '\0') {
                stop++;
            }
            const size_t s = stop - start;
            if (s) {
                memcpy(cur_write, &cur_path[start], s);
                cur_write += s;

                // If last symbol written was not a directory symbol, add it.
                if (*(cur_write - 1) != DIRECTORY_SYMBOL) {
                    *cur_write++ = DIRECTORY_SYMBOL;
                }

                if (relative_path_size > 0) {
                    memcpy(cur_write, relative_path, relative_path_size);
                    cur_write += relative_path_size;
                }
                *cur_write++ = PATH_SEPARATOR;
                start = stop;
            }
        }
        *output_path = cur_write;
    }
}

// Check to see if there's enough space in the data file list.  If not, add some.
static inline VkResult check_and_adjust_data_file_list(const struct loader_instance *inst, struct loader_data_files *out_files) {
    if (out_files->count == 0) {
        out_files->filename_list = loader_instance_heap_alloc(inst, 64 * sizeof(char *), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (NULL == out_files->filename_list) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "check_and_adjust_data_file_list: Failed to allocate space for manifest file name list");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        out_files->alloc_count = 64;
    } else if (out_files->count == out_files->alloc_count) {
        size_t new_size = out_files->alloc_count * sizeof(char *) * 2;
        void *new_ptr = loader_instance_heap_realloc(inst, out_files->filename_list, out_files->alloc_count * sizeof(char *),
                                                     new_size, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (NULL == new_ptr) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "check_and_adjust_data_file_list: Failed to reallocate space for manifest file name list");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        out_files->filename_list = new_ptr;
        out_files->alloc_count *= 2;
    }

    return VK_SUCCESS;
}

// add file_name to the out_files manifest list. Assumes its a valid manifest file name
static VkResult add_manifest_file(const struct loader_instance *inst, const char *file_name, struct loader_data_files *out_files) {
    VkResult vk_result = VK_SUCCESS;

    // Check and allocate space in the manifest list if necessary
    vk_result = check_and_adjust_data_file_list(inst, out_files);
    if (VK_SUCCESS != vk_result) {
        goto out;
    }

    out_files->filename_list[out_files->count] =
        loader_instance_heap_alloc(inst, strlen(file_name) + 1, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (out_files->filename_list[out_files->count] == NULL) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "add_manifest_file: Failed to allocate space for manifest file %d list",
                   out_files->count);
        vk_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }

    strcpy(out_files->filename_list[out_files->count++], file_name);

out:
    return vk_result;
}

// If the file found is a manifest file name, add it to the out_files manifest list.
static VkResult add_if_manifest_file(const struct loader_instance *inst, const char *file_name,
                                     struct loader_data_files *out_files) {
    VkResult vk_result = VK_SUCCESS;

    assert(NULL != file_name && "add_if_manifest_file: Received NULL pointer for file_name");
    assert(NULL != out_files && "add_if_manifest_file: Received NULL pointer for out_files");

    // Look for files ending with ".json" suffix
    size_t name_len = strlen(file_name);
    const char *name_suffix = file_name + name_len - 5;
    if ((name_len < 5) || 0 != strncmp(name_suffix, ".json", 5)) {
        // Use incomplete to indicate invalid name, but to keep going.
        vk_result = VK_INCOMPLETE;
        goto out;
    }

    vk_result = add_manifest_file(inst, file_name, out_files);

out:

    return vk_result;
}

VkResult add_data_files_in_path(const struct loader_instance *inst, char *search_path, bool is_directory_list,
                                struct loader_data_files *out_files, bool use_first_found_manifest) {
    VkResult vk_result = VK_SUCCESS;
    DIR *dir_stream = NULL;
    struct dirent *dir_entry;
    char *cur_file;
    char *next_file;
    char *name;
    char full_path[2048];
#ifndef _WIN32
    char temp_path[2048];
#endif

    // Now, parse the paths
    next_file = search_path;
    while (NULL != next_file && *next_file != '\0') {
        name = NULL;
        cur_file = next_file;
        next_file = loader_get_next_path(cur_file);

        // Get the next name in the list and verify it's valid
        if (is_directory_list) {
            dir_stream = loader_opendir(inst, cur_file);
            if (NULL == dir_stream) {
                continue;
            }
            while (1) {
                dir_entry = readdir(dir_stream);
                if (NULL == dir_entry) {
                    break;
                }

                name = &(dir_entry->d_name[0]);
                loader_get_fullpath(name, cur_file, sizeof(full_path), full_path);
                name = full_path;

                VkResult local_res;
                local_res = add_if_manifest_file(inst, name, out_files);

                // Incomplete means this was not a valid data file.
                if (local_res == VK_INCOMPLETE) {
                    continue;
                } else if (local_res != VK_SUCCESS) {
                    vk_result = local_res;
                    break;
                }
            }
            loader_closedir(inst, dir_stream);
            if (vk_result != VK_SUCCESS) {
                goto out;
            }
        } else {
#ifdef _WIN32
            name = cur_file;
#else
            // Only Linux has relative paths, make a copy of location so it isn't modified
            size_t str_len;
            if (NULL != next_file) {
                str_len = next_file - cur_file + 1;
            } else {
                str_len = strlen(cur_file) + 1;
            }
            if (str_len > sizeof(temp_path)) {
                loader_log(inst, VULKAN_LOADER_DEBUG_BIT, 0, "add_data_files_in_path: Path to %s too long\n", cur_file);
                continue;
            }
            strcpy(temp_path, cur_file);
            name = temp_path;
#endif
            loader_get_fullpath(cur_file, name, sizeof(full_path), full_path);
            name = full_path;

            VkResult local_res;
            local_res = add_if_manifest_file(inst, name, out_files);

            // Incomplete means this was not a valid data file.
            if (local_res == VK_INCOMPLETE) {
                continue;
            } else if (local_res != VK_SUCCESS) {
                vk_result = local_res;
                break;
            }
        }
        if (use_first_found_manifest && out_files->count > 0) {
            break;
        }
    }

out:

    return vk_result;
}

// Look for data files in the provided paths, but first check the environment override to determine if we should use that
// instead.
static VkResult read_data_files_in_search_paths(const struct loader_instance *inst, enum loader_data_files_type data_file_type,
                                                const char *env_override, const char *path_override, const char *relative_location,
                                                bool *override_active, struct loader_data_files *out_files) {
    VkResult vk_result = VK_SUCCESS;
    bool is_directory_list = true;
    bool is_icd = (data_file_type == LOADER_DATA_FILE_MANIFEST_ICD);
    char *override_env = NULL;
    const char *override_path = NULL;
    size_t search_path_size = 0;
    char *search_path = NULL;
    char *cur_path_ptr = NULL;
    size_t rel_size = 0;
    bool use_first_found_manifest = false;
#ifndef _WIN32
    bool xdg_config_home_secenv_alloc = true;
    bool xdg_config_dirs_secenv_alloc = true;
    bool xdg_data_home_secenv_alloc = true;
    bool xdg_data_dirs_secenv_alloc = true;
#endif

#ifndef _WIN32
    // Determine how much space is needed to generate the full search path
    // for the current manifest files.
    char *xdg_config_home = loader_secure_getenv("XDG_CONFIG_HOME", inst);
    if (NULL == xdg_config_home) {
        xdg_config_home_secenv_alloc = false;
    }

    char *xdg_config_dirs = loader_secure_getenv("XDG_CONFIG_DIRS", inst);
    if (NULL == xdg_config_dirs) {
        xdg_config_dirs_secenv_alloc = false;
    }
#if !defined(__Fuchsia__) && !defined(__QNXNTO__)
    if (NULL == xdg_config_dirs || '\0' == xdg_config_dirs[0]) {
        xdg_config_dirs = FALLBACK_CONFIG_DIRS;
    }
#endif

    char *xdg_data_home = loader_secure_getenv("XDG_DATA_HOME", inst);
    if (NULL == xdg_data_home) {
        xdg_data_home_secenv_alloc = false;
    }

    char *xdg_data_dirs = loader_secure_getenv("XDG_DATA_DIRS", inst);
    if (NULL == xdg_data_dirs) {
        xdg_data_dirs_secenv_alloc = false;
    }
#if !defined(__Fuchsia__) && !defined(__QNXNTO__)
    if (NULL == xdg_data_dirs || '\0' == xdg_data_dirs[0]) {
        xdg_data_dirs = FALLBACK_DATA_DIRS;
    }
#endif

    char *home = NULL;
    char *default_data_home = NULL;
    char *default_config_home = NULL;

    // Only use HOME if XDG_DATA_HOME is not present on the system
    home = loader_secure_getenv("HOME", inst);
    if (home != NULL) {
        if (NULL == xdg_config_home || '\0' == xdg_config_home[0]) {
            const char config_suffix[] = "/.config";
            default_config_home =
                loader_instance_heap_alloc(inst, strlen(home) + strlen(config_suffix) + 1, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
            if (default_config_home == NULL) {
                vk_result = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto out;
            }
            strcpy(default_config_home, home);
            strcat(default_config_home, config_suffix);
        }
        if (NULL == xdg_data_home || '\0' == xdg_data_home[0]) {
            const char data_suffix[] = "/.local/share";
            default_data_home =
                loader_instance_heap_alloc(inst, strlen(home) + strlen(data_suffix) + 1, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
            if (default_data_home == NULL) {
                vk_result = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto out;
            }
            strcpy(default_data_home, home);
            strcat(default_data_home, data_suffix);
        }
    }
#endif  // !_WIN32

    if (path_override != NULL) {
        override_path = path_override;
    } else if (env_override != NULL) {
#ifndef _WIN32
        if (geteuid() != getuid() || getegid() != getgid()) {
            // Don't allow setuid apps to use the env var:
            env_override = NULL;
        } else
#endif
        {
            override_env = loader_secure_getenv(env_override, inst);

            // The ICD override is actually a specific list of filenames, not directories
            if (is_icd && NULL != override_env) {
                is_directory_list = false;
            }
            override_path = override_env;
        }
    }

    // Add two by default for NULL terminator and one path separator on end (just in case)
    search_path_size = 2;

    // If there's an override, use that (and the local folder if required) and nothing else
    if (NULL != override_path) {
        // Local folder and null terminator
        search_path_size += strlen(override_path) + 1;
    } else if (NULL == relative_location) {
        // If there's no override, and no relative location, bail out.  This is usually
        // the case when we're on Windows and the default path is to use the registry.
        goto out;
    } else {
        // Add the general search folders (with the appropriate relative folder added)
        rel_size = strlen(relative_location);
        if (rel_size == 0) {
            goto out;
        } else {
#if defined(__APPLE__)
            search_path_size += MAXPATHLEN;
#endif
#ifndef _WIN32
            // Only add the home folders if not ICD filenames or superuser
            if (is_directory_list && !is_high_integrity()) {
                if (NULL != default_config_home) {
                    search_path_size += determine_data_file_path_size(default_config_home, rel_size);
                } else {
                    search_path_size += determine_data_file_path_size(xdg_config_home, rel_size);
                }
            }
            search_path_size += determine_data_file_path_size(xdg_config_dirs, rel_size);
            search_path_size += determine_data_file_path_size(SYSCONFDIR, rel_size);
#if defined(EXTRASYSCONFDIR)
            search_path_size += determine_data_file_path_size(EXTRASYSCONFDIR, rel_size);
#endif
            // Only add the home folders if not ICD filenames or superuser
            if (is_directory_list && !is_high_integrity()) {
                if (NULL != default_data_home) {
                    search_path_size += determine_data_file_path_size(default_data_home, rel_size);
                } else {
                    search_path_size += determine_data_file_path_size(xdg_data_home, rel_size);
                }
            }
            search_path_size += determine_data_file_path_size(xdg_data_dirs, rel_size);
#endif  // !_WIN32
        }
    }

    // Allocate the required space
    search_path = loader_instance_heap_alloc(inst, search_path_size, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
    if (NULL == search_path) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "read_data_files_in_search_paths: Failed to allocate space for search path of length %d",
                   (uint32_t)search_path_size);
        vk_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }

    cur_path_ptr = search_path;

    // Add the remaining paths to the list
    if (NULL != override_path) {
        strcpy(cur_path_ptr, override_path);
    } else {
#ifndef _WIN32
        if (rel_size > 0) {
#if defined(__APPLE__)
            // Add the bundle's Resources dir to the beginning of the search path.
            // Looks for manifests in the bundle first, before any system directories.
            CFBundleRef main_bundle = CFBundleGetMainBundle();
            if (NULL != main_bundle) {
                CFURLRef ref = CFBundleCopyResourcesDirectoryURL(main_bundle);
                if (NULL != ref) {
                    if (CFURLGetFileSystemRepresentation(ref, TRUE, (UInt8 *)cur_path_ptr, search_path_size)) {
                        cur_path_ptr += strlen(cur_path_ptr);
                        *cur_path_ptr++ = DIRECTORY_SYMBOL;
                        memcpy(cur_path_ptr, relative_location, rel_size);
                        cur_path_ptr += rel_size;
                        *cur_path_ptr++ = PATH_SEPARATOR;
                        // only for ICD manifests
                        if (env_override != NULL && strcmp(VK_ICD_FILENAMES_ENV_VAR, env_override) == 0) {
                            use_first_found_manifest = true;
                        }
                    }
                    CFRelease(ref);
                }
            }
#endif  // __APPLE__
        // Only add the home folders if not ICD filenames or superuser
            if (is_directory_list && !is_high_integrity()) {
                if (NULL != default_config_home) {
                    copy_data_file_path(default_config_home, relative_location, rel_size, &cur_path_ptr);
                } else {
                    copy_data_file_path(xdg_config_home, relative_location, rel_size, &cur_path_ptr);
                }
            }
            copy_data_file_path(xdg_config_dirs, relative_location, rel_size, &cur_path_ptr);
            copy_data_file_path(SYSCONFDIR, relative_location, rel_size, &cur_path_ptr);
#if defined(EXTRASYSCONFDIR)
            copy_data_file_path(EXTRASYSCONFDIR, relative_location, rel_size, &cur_path_ptr);
#endif

            // Only add the home folders if not ICD filenames or superuser
            if (is_directory_list && !is_high_integrity()) {
                if (NULL != default_data_home) {
                    copy_data_file_path(default_data_home, relative_location, rel_size, &cur_path_ptr);
                } else {
                    copy_data_file_path(xdg_data_home, relative_location, rel_size, &cur_path_ptr);
                }
            }
            copy_data_file_path(xdg_data_dirs, relative_location, rel_size, &cur_path_ptr);
        }

        // Remove the last path separator
        --cur_path_ptr;

        assert(cur_path_ptr - search_path < (ptrdiff_t)search_path_size);
        *cur_path_ptr = '\0';
#endif  // !_WIN32
    }

    // Remove duplicate paths, or it would result in duplicate extensions, duplicate devices, etc.
    // This uses minimal memory, but is O(N^2) on the number of paths. Expect only a few paths.
    char path_sep_str[2] = {PATH_SEPARATOR, '\0'};
    size_t search_path_updated_size = strlen(search_path);
    for (size_t first = 0; first < search_path_updated_size;) {
        // If this is an empty path, erase it
        if (search_path[first] == PATH_SEPARATOR) {
            memmove(&search_path[first], &search_path[first + 1], search_path_updated_size - first + 1);
            search_path_updated_size -= 1;
            continue;
        }

        size_t first_end = first + 1;
        first_end += strcspn(&search_path[first_end], path_sep_str);
        for (size_t second = first_end + 1; second < search_path_updated_size;) {
            size_t second_end = second + 1;
            second_end += strcspn(&search_path[second_end], path_sep_str);
            if (first_end - first == second_end - second &&
                !strncmp(&search_path[first], &search_path[second], second_end - second)) {
                // Found duplicate. Include PATH_SEPARATOR in second_end, then erase it from search_path.
                if (search_path[second_end] == PATH_SEPARATOR) {
                    second_end++;
                }
                memmove(&search_path[second], &search_path[second_end], search_path_updated_size - second_end + 1);
                search_path_updated_size -= second_end - second;
            } else {
                second = second_end + 1;
            }
        }
        first = first_end + 1;
    }
    search_path_size = search_path_updated_size;

    // Print out the paths being searched if debugging is enabled
    uint32_t log_flags = 0;
    if (search_path_size > 0) {
        char *tmp_search_path = loader_instance_heap_alloc(inst, search_path_size + 1, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
        if (NULL != tmp_search_path) {
            strncpy(tmp_search_path, search_path, search_path_size);
            tmp_search_path[search_path_size] = '\0';
            if (data_file_type == LOADER_DATA_FILE_MANIFEST_ICD) {
                log_flags = VULKAN_LOADER_DRIVER_BIT;
                loader_log(inst, VULKAN_LOADER_DRIVER_BIT, 0, "Searching for driver manifest files");
            } else {
                log_flags = VULKAN_LOADER_LAYER_BIT;
                loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "Searching for layer manifest files");
            }
            loader_log(inst, log_flags, 0, "   In following folders:");
            char *cur_file;
            char *next_file = tmp_search_path;
            while (NULL != next_file && *next_file != '\0') {
                cur_file = next_file;
                next_file = loader_get_next_path(cur_file);
                loader_log(inst, log_flags, 0, "      %s", cur_file);
            }
            loader_instance_heap_free(inst, tmp_search_path);
        }
    }

    // Now, parse the paths and add any manifest files found in them.
    vk_result = add_data_files_in_path(inst, search_path, is_directory_list, out_files, use_first_found_manifest);

    if (log_flags != 0 && out_files->count > 0) {
        loader_log(inst, log_flags, 0, "   Found the following files:");
        for (uint32_t cur_file = 0; cur_file < out_files->count; ++cur_file) {
            loader_log(inst, log_flags, 0, "      %s", out_files->filename_list[cur_file]);
        }
    } else {
        loader_log(inst, log_flags, 0, "   Found no files");
    }

    if (NULL != override_path) {
        *override_active = true;
    } else {
        *override_active = false;
    }

out:

    if (NULL != override_env) {
        loader_free_getenv(override_env, inst);
    }
#ifndef _WIN32
    if (xdg_config_home_secenv_alloc) {
        loader_free_getenv(xdg_config_home, inst);
    }
    if (xdg_config_dirs_secenv_alloc) {
        loader_free_getenv(xdg_config_dirs, inst);
    }
    if (xdg_data_home_secenv_alloc) {
        loader_free_getenv(xdg_data_home, inst);
    }
    if (xdg_data_dirs_secenv_alloc) {
        loader_free_getenv(xdg_data_dirs, inst);
    }
    if (NULL != xdg_data_home) {
        loader_free_getenv(xdg_data_home, inst);
    }
    if (NULL != home) {
        loader_free_getenv(home, inst);
    }
    if (NULL != default_data_home) {
        loader_instance_heap_free(inst, default_data_home);
    }
    if (NULL != default_config_home) {
        loader_instance_heap_free(inst, default_config_home);
    }
#endif

    if (NULL != search_path) {
        loader_instance_heap_free(inst, search_path);
    }

    return vk_result;
}

// Find the Vulkan library manifest files.
//
// This function scans the "location" or "env_override" directories/files
// for a list of JSON manifest files.  If env_override is non-NULL
// and has a valid value. Then the location is ignored.  Otherwise
// location is used to look for manifest files. The location
// is interpreted as  Registry path on Windows and a directory path(s)
// on Linux. "home_location" is an additional directory in the users home
// directory to look at. It is expanded into the dir path
// $XDG_DATA_HOME/home_location or $HOME/.local/share/home_location depending
// on environment variables. This "home_location" is only used on Linux.
//
// \returns
// VKResult
// A string list of manifest files to be opened in out_files param.
// List has a pointer to string for each manifest filename.
// When done using the list in out_files, pointers should be freed.
// Location or override  string lists can be either files or directories as
// follows:
//            | location | override
// --------------------------------
// Win ICD    | files    | files
// Win Layer  | files    | dirs
// Linux ICD  | dirs     | files
// Linux Layer| dirs     | dirs
VkResult loader_get_data_files(const struct loader_instance *inst, enum loader_data_files_type data_file_type,
                               bool warn_if_not_present, const char *env_override, const char *path_override,
                               char *registry_location, const char *relative_location, struct loader_data_files *out_files) {
    VkResult res = VK_SUCCESS;
    bool override_active = false;

    // Free and init the out_files information so there's no false data left from uninitialized variables.
    if (out_files->filename_list != NULL) {
        for (uint32_t i = 0; i < out_files->count; i++) {
            if (NULL != out_files->filename_list[i]) {
                loader_instance_heap_free(inst, out_files->filename_list[i]);
                out_files->filename_list[i] = NULL;
            }
        }
        loader_instance_heap_free(inst, out_files->filename_list);
    }
    out_files->count = 0;
    out_files->alloc_count = 0;
    out_files->filename_list = NULL;

    res = read_data_files_in_search_paths(inst, data_file_type, env_override, path_override, relative_location, &override_active,
                                          out_files);
    if (VK_SUCCESS != res) {
        goto out;
    }

#ifdef _WIN32
    // Read the registry if the override wasn't active.
    if (!override_active) {
        res = windows_read_data_files_in_registry(inst, data_file_type, warn_if_not_present, registry_location, out_files);
        if (VK_SUCCESS != res) {
            goto out;
        }
    }
#endif

out:

    if (VK_SUCCESS != res && NULL != out_files->filename_list) {
        for (uint32_t remove = 0; remove < out_files->count; remove++) {
            loader_instance_heap_free(inst, out_files->filename_list[remove]);
        }
        loader_instance_heap_free(inst, out_files->filename_list);
        out_files->count = 0;
        out_files->alloc_count = 0;
        out_files->filename_list = NULL;
    }

    return res;
}

void loader_init_icd_lib_list() {}

void loader_destroy_icd_lib_list() {}

// Try to find the Vulkan ICD driver(s).
//
// This function scans the default system loader path(s) or path
// specified by the \c VK_ICD_FILENAMES environment variable in
// order to find loadable VK ICDs manifest files. From these
// manifest files it finds the ICD libraries.
//
// \returns
// Vulkan result
// (on result == VK_SUCCESS) a list of icds that were discovered
VkResult loader_icd_scan(const struct loader_instance *inst, struct loader_icd_tramp_list *icd_tramp_list) {
    char *file_str;
    uint16_t file_major_vers = 0;
    uint16_t file_minor_vers = 0;
    uint16_t file_patch_vers = 0;
    char *vers_tok;
    struct loader_data_files manifest_files;
    VkResult res = VK_SUCCESS;
    bool lockedMutex = false;
    cJSON *json = NULL;
    uint32_t num_good_icds = 0;

    memset(&manifest_files, 0, sizeof(struct loader_data_files));

    res = loader_scanned_icd_init(inst, icd_tramp_list);
    if (VK_SUCCESS != res) {
        goto out;
    }
    // Get a list of manifest files for ICDs
    res = loader_get_data_files(inst, LOADER_DATA_FILE_MANIFEST_ICD, true, VK_ICD_FILENAMES_ENV_VAR, NULL,
                                VK_DRIVERS_INFO_REGISTRY_LOC, VK_DRIVERS_INFO_RELATIVE_DIR, &manifest_files);
    if (VK_SUCCESS != res || manifest_files.count == 0) {
        goto out;
    }
    loader_platform_thread_lock_mutex(&loader_json_lock);
    lockedMutex = true;
    for (uint32_t i = 0; i < manifest_files.count; i++) {
        file_str = manifest_files.filename_list[i];
        if (file_str == NULL) {
            continue;
        }

        VkResult temp_res = loader_get_json(inst, file_str, &json);
        if (NULL == json || temp_res != VK_SUCCESS) {
            if (NULL != json) {
                cJSON_Delete(inst, json);
                json = NULL;
            }
            // If we haven't already found an ICD, copy this result to
            // the returned result.
            if (num_good_icds == 0) {
                res = temp_res;
            }
            if (temp_res == VK_ERROR_OUT_OF_HOST_MEMORY) {
                res = VK_ERROR_OUT_OF_HOST_MEMORY;
                break;
            } else {
                continue;
            }
        }
        res = temp_res;

        cJSON *item, *itemICD;
        item = cJSON_GetObjectItem(json, "file_format_version");
        if (item == NULL) {
            if (num_good_icds == 0) {
                res = VK_ERROR_INITIALIZATION_FAILED;
            }
            loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                       "loader_icd_scan: ICD JSON %s does not have a \'file_format_version\' field. Skipping ICD JSON.", file_str);
            cJSON_Delete(inst, json);
            json = NULL;
            continue;
        }

        char *file_vers = cJSON_Print(inst, item);
        if (NULL == file_vers) {
            // Only reason the print can fail is if there was an allocation issue
            if (num_good_icds == 0) {
                res = VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                       "loader_icd_scan: Failed retrieving ICD JSON %s \'file_format_version\' field.  Skipping ICD JSON",
                       file_str);
            cJSON_Delete(inst, json);
            json = NULL;
            continue;
        }
        loader_log(inst, VULKAN_LOADER_INFO_BIT, 0, "Found ICD manifest file %s, version %s", file_str, file_vers);

        // Get the major/minor/and patch as integers for easier comparison
        vers_tok = strtok(file_vers, ".\"\n\r");
        if (NULL != vers_tok) {
            file_major_vers = (uint16_t)atoi(vers_tok);
            vers_tok = strtok(NULL, ".\"\n\r");
            if (NULL != vers_tok) {
                file_minor_vers = (uint16_t)atoi(vers_tok);
                vers_tok = strtok(NULL, ".\"\n\r");
                if (NULL != vers_tok) {
                    file_patch_vers = (uint16_t)atoi(vers_tok);
                }
            }
        }

        if (file_major_vers != 1 || file_minor_vers != 0 || file_patch_vers > 1) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                       "loader_icd_scan: Unexpected manifest file version (expected 1.0.0 or 1.0.1), may cause errors");
        }
        cJSON_Free(inst, file_vers);

        itemICD = cJSON_GetObjectItem(json, "ICD");
        if (itemICD != NULL) {
            item = cJSON_GetObjectItem(itemICD, "library_path");
            if (item != NULL) {
                char *temp = cJSON_Print(inst, item);
                if (!temp || strlen(temp) == 0) {
                    if (num_good_icds == 0) {
                        res = VK_ERROR_OUT_OF_HOST_MEMORY;
                    }
                    loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                               "loader_icd_scan: Failed retrieving ICD JSON %s \'library_path\' field.  Skipping ICD JSON.",
                               file_str);
                    cJSON_Free(inst, temp);
                    cJSON_Delete(inst, json);
                    json = NULL;
                    continue;
                }
                // strip out extra quotes
                temp[strlen(temp) - 1] = '\0';
                char *library_path = loader_stack_alloc(strlen(temp) + 1);
                if (NULL == library_path) {
                    loader_log(
                        inst, VULKAN_LOADER_ERROR_BIT, 0,
                        "loader_icd_scan: Failed to allocate space for ICD JSON %s \'library_path\' value.  Skipping ICD JSON.",
                        file_str);
                    res = VK_ERROR_OUT_OF_HOST_MEMORY;
                    cJSON_Free(inst, temp);
                    cJSON_Delete(inst, json);
                    json = NULL;
                    goto out;
                }
                strcpy(library_path, &temp[1]);
                cJSON_Free(inst, temp);
                if (strlen(library_path) == 0) {
                    loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                               "loader_icd_scan: ICD JSON %s \'library_path\' field is empty.  Skipping ICD JSON.", file_str);
                    cJSON_Delete(inst, json);
                    json = NULL;
                    continue;
                }
                char fullpath[MAX_STRING_SIZE];
                // Print out the paths being searched if debugging is enabled
                loader_log(inst, VULKAN_LOADER_DEBUG_BIT, 0, "Searching for ICD drivers named %s", library_path);
                if (loader_platform_is_path(library_path)) {
                    // a relative or absolute path
                    char *name_copy = loader_stack_alloc(strlen(file_str) + 1);
                    char *rel_base;
                    strcpy(name_copy, file_str);
                    rel_base = loader_platform_dirname(name_copy);
                    loader_expand_path(library_path, rel_base, sizeof(fullpath), fullpath);
                } else {
// a filename which is assumed in a system directory
#if defined(DEFAULT_VK_DRIVERS_PATH)
                    loader_get_fullpath(library_path, DEFAULT_VK_DRIVERS_PATH, sizeof(fullpath), fullpath);
#else
                    loader_get_fullpath(library_path, "", sizeof(fullpath), fullpath);
#endif
                }

                uint32_t vers = 0;
                item = cJSON_GetObjectItem(itemICD, "api_version");
                if (item != NULL) {
                    temp = cJSON_Print(inst, item);
                    if (NULL == temp) {
                        loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                                   "loader_icd_scan: Failed retrieving ICD JSON %s \'api_version\' field.  Skipping ICD JSON.",
                                   file_str);

                        // Only reason the print can fail is if there was an
                        // allocation issue
                        if (num_good_icds == 0) {
                            res = VK_ERROR_OUT_OF_HOST_MEMORY;
                        }

                        cJSON_Free(inst, temp);
                        cJSON_Delete(inst, json);
                        json = NULL;
                        continue;
                    }
                    vers = loader_make_version(temp);
                    cJSON_Free(inst, temp);
                } else {
                    loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                               "loader_icd_scan: ICD JSON %s does not have an \'api_version\' field.", file_str);
                }
                VkResult icd_add_res = VK_SUCCESS;
                icd_add_res = loader_scanned_icd_add(inst, icd_tramp_list, fullpath, vers);
                if (VK_ERROR_OUT_OF_HOST_MEMORY == icd_add_res) {
                    res = icd_add_res;
                    goto out;
                } else if (VK_SUCCESS != icd_add_res) {
                    loader_log(inst, VULKAN_LOADER_ERROR_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                               "loader_icd_scan: Failed to add ICD JSON %s.  Skipping ICD JSON.", fullpath);
                    cJSON_Delete(inst, json);
                    json = NULL;
                    continue;
                }
                num_good_icds++;
            } else {
                loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                           "loader_icd_scan: Failed to find \'library_path\' object in ICD JSON file %s.  Skipping ICD JSON.",
                           file_str);
            }
        } else {
            loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                       "loader_icd_scan: Can not find \'ICD\' object in ICD JSON file %s.  Skipping ICD JSON", file_str);
        }

        cJSON_Delete(inst, json);
        json = NULL;
    }

out:

    if (NULL != json) {
        cJSON_Delete(inst, json);
    }

    if (NULL != manifest_files.filename_list) {
        for (uint32_t i = 0; i < manifest_files.count; i++) {
            if (NULL != manifest_files.filename_list[i]) {
                loader_instance_heap_free(inst, manifest_files.filename_list[i]);
            }
        }
        loader_instance_heap_free(inst, manifest_files.filename_list);
    }
    if (lockedMutex) {
        loader_platform_thread_unlock_mutex(&loader_json_lock);
    }

    return res;
}

void loader_scan_for_layers(struct loader_instance *inst, struct loader_layer_list *instance_layers) {
    char *file_str;
    struct loader_data_files manifest_files;
    cJSON *json;
    bool override_layer_valid = false;
    char *override_paths = NULL;
    uint32_t total_count = 0;

    memset(&manifest_files, 0, sizeof(struct loader_data_files));

    // Cleanup any previously scanned libraries
    loader_delete_layer_list_and_properties(inst, instance_layers);

    loader_platform_thread_lock_mutex(&loader_json_lock);

    // Get a list of manifest files for any implicit layers
    // Pass NULL for environment variable override - implicit layers are not overridden by LAYERS_PATH_ENV
    if (VK_SUCCESS != loader_get_data_files(inst, LOADER_DATA_FILE_MANIFEST_LAYER, false, NULL, NULL, VK_ILAYERS_INFO_REGISTRY_LOC,
                                            VK_ILAYERS_INFO_RELATIVE_DIR, &manifest_files)) {
        goto out;
    }

    if (manifest_files.count != 0) {
        total_count += manifest_files.count;
        for (uint32_t i = 0; i < manifest_files.count; i++) {
            file_str = manifest_files.filename_list[i];
            if (file_str == NULL) {
                continue;
            }

            // Parse file into JSON struct
            VkResult res = loader_get_json(inst, file_str, &json);
            if (VK_ERROR_OUT_OF_HOST_MEMORY == res) {
                goto out;
            } else if (VK_SUCCESS != res || NULL == json) {
                continue;
            }

            VkResult local_res = loader_add_layer_properties(inst, instance_layers, json, true, file_str);
            cJSON_Delete(inst, json);

            // If the error is anything other than out of memory we still want to try to load the other layers
            if (VK_ERROR_OUT_OF_HOST_MEMORY == local_res) {
                goto out;
            }
        }
    }

    // Remove any extraneous override layers.
    remove_all_non_valid_override_layers(inst, instance_layers);

    // Check to see if the override layer is present, and use it's override paths.
    for (int32_t i = 0; i < (int32_t)instance_layers->count; i++) {
        struct loader_layer_properties *prop = &instance_layers->list[i];
        if (prop->is_override && loader_implicit_layer_is_enabled(inst, prop) && prop->num_override_paths > 0) {
            char *cur_write_ptr = NULL;
            size_t override_path_size = 0;
            for (uint32_t j = 0; j < prop->num_override_paths; j++) {
                override_path_size += determine_data_file_path_size(prop->override_paths[j], 0);
            }
            override_paths = loader_instance_heap_alloc(inst, override_path_size, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
            if (override_paths == NULL) {
                goto out;
            }
            cur_write_ptr = &override_paths[0];
            for (uint32_t j = 0; j < prop->num_override_paths; j++) {
                copy_data_file_path(prop->override_paths[j], NULL, 0, &cur_write_ptr);
            }
            // Remove the last path separator
            --cur_write_ptr;
            assert(cur_write_ptr - override_paths < (ptrdiff_t)override_path_size);
            *cur_write_ptr = '\0';
            loader_log(NULL, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                       "loader_scan_for_layers: Override layer has override paths set to %s", override_paths);
        }
    }

    // Get a list of manifest files for explicit layers
    if (VK_SUCCESS != loader_get_data_files(inst, LOADER_DATA_FILE_MANIFEST_LAYER, true, VK_LAYER_PATH_ENV_VAR, override_paths,
                                            VK_ELAYERS_INFO_REGISTRY_LOC, VK_ELAYERS_INFO_RELATIVE_DIR, &manifest_files)) {
        goto out;
    }

    // Make sure we have at least one layer, if not, go ahead and return
    if (manifest_files.count == 0 && total_count == 0) {
        goto out;
    } else {
        for (uint32_t i = 0; i < manifest_files.count; i++) {
            file_str = manifest_files.filename_list[i];
            if (file_str == NULL) {
                continue;
            }

            // Parse file into JSON struct
            VkResult res = loader_get_json(inst, file_str, &json);
            if (VK_ERROR_OUT_OF_HOST_MEMORY == res) {
                goto out;
            } else if (VK_SUCCESS != res || NULL == json) {
                continue;
            }

            VkResult local_res = loader_add_layer_properties(inst, instance_layers, json, false, file_str);
            cJSON_Delete(inst, json);

            // If the error is anything other than out of memory we still want to try to load the other layers
            if (VK_ERROR_OUT_OF_HOST_MEMORY == local_res) {
                goto out;
            }
        }
    }

    // Verify any meta-layers in the list are valid and all the component layers are
    // actually present in the available layer list
    verify_all_meta_layers(inst, instance_layers, &override_layer_valid);

    if (override_layer_valid) {
        loader_remove_layers_in_blacklist(inst, instance_layers);
        if (NULL != inst) {
            inst->override_layer_present = true;
        }
    }

out:

    if (NULL != override_paths) {
        loader_instance_heap_free(inst, override_paths);
    }
    if (NULL != manifest_files.filename_list) {
        for (uint32_t i = 0; i < manifest_files.count; i++) {
            if (NULL != manifest_files.filename_list[i]) {
                loader_instance_heap_free(inst, manifest_files.filename_list[i]);
            }
        }
        loader_instance_heap_free(inst, manifest_files.filename_list);
    }
    loader_platform_thread_unlock_mutex(&loader_json_lock);
}

void loader_scan_for_implicit_layers(struct loader_instance *inst, struct loader_layer_list *instance_layers) {
    char *file_str;
    struct loader_data_files manifest_files;
    cJSON *json;
    bool override_layer_valid = false;
    char *override_paths = NULL;
    bool implicit_metalayer_present = false;
    bool have_json_lock = false;

    // Before we begin anything, init manifest_files to avoid a delete of garbage memory if
    // a failure occurs before allocating the manifest filename_list.
    memset(&manifest_files, 0, sizeof(struct loader_data_files));

    // Pass NULL for environment variable override - implicit layers are not overridden by LAYERS_PATH_ENV
    VkResult res = loader_get_data_files(inst, LOADER_DATA_FILE_MANIFEST_LAYER, false, NULL, NULL, VK_ILAYERS_INFO_REGISTRY_LOC,
                                         VK_ILAYERS_INFO_RELATIVE_DIR, &manifest_files);
    if (VK_SUCCESS != res || manifest_files.count == 0) {
        goto out;
    }

    // Cleanup any previously scanned libraries
    loader_delete_layer_list_and_properties(inst, instance_layers);

    loader_platform_thread_lock_mutex(&loader_json_lock);
    have_json_lock = true;

    for (uint32_t i = 0; i < manifest_files.count; i++) {
        file_str = manifest_files.filename_list[i];
        if (file_str == NULL) {
            continue;
        }

        // parse file into JSON struct
        res = loader_get_json(inst, file_str, &json);
        if (VK_ERROR_OUT_OF_HOST_MEMORY == res) {
            goto out;
        } else if (VK_SUCCESS != res || NULL == json) {
            continue;
        }

        res = loader_add_layer_properties(inst, instance_layers, json, true, file_str);

        loader_instance_heap_free(inst, file_str);
        manifest_files.filename_list[i] = NULL;
        cJSON_Delete(inst, json);

        if (VK_ERROR_OUT_OF_HOST_MEMORY == res) {
            goto out;
        }
    }

    // Remove any extraneous override layers.
    remove_all_non_valid_override_layers(inst, instance_layers);

    // Check to see if either the override layer is present, or another implicit meta-layer.
    // Each of these may require explicit layers to be enabled at this time.
    for (int32_t i = 0; i < (int32_t)instance_layers->count; i++) {
        struct loader_layer_properties *prop = &instance_layers->list[i];
        if (prop->is_override && loader_implicit_layer_is_enabled(inst, prop)) {
            override_layer_valid = true;
            if (prop->num_override_paths > 0) {
                char *cur_write_ptr = NULL;
                size_t override_path_size = 0;
                for (uint32_t j = 0; j < prop->num_override_paths; j++) {
                    override_path_size += determine_data_file_path_size(prop->override_paths[j], 0);
                }
                override_paths = loader_instance_heap_alloc(inst, override_path_size, VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
                if (override_paths == NULL) {
                    goto out;
                }
                cur_write_ptr = &override_paths[0];
                for (uint32_t j = 0; j < prop->num_override_paths; j++) {
                    copy_data_file_path(prop->override_paths[j], NULL, 0, &cur_write_ptr);
                }
                // Remove the last path separator
                --cur_write_ptr;
                assert(cur_write_ptr - override_paths < (ptrdiff_t)override_path_size);
                *cur_write_ptr = '\0';
                loader_log(NULL, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                           "loader_scan_for_implicit_layers: Override layer has override paths set to %s", override_paths);
            }
        } else if (!prop->is_override && prop->type_flags & VK_LAYER_TYPE_FLAG_META_LAYER) {
            implicit_metalayer_present = true;
        }
    }

    // If either the override layer or an implicit meta-layer are present, we need to add
    // explicit layer info as well.  Not to worry, though, all explicit layers not included
    // in the override layer will be removed below in loader_remove_layers_in_blacklist().
    if (override_layer_valid || implicit_metalayer_present) {
        if (VK_SUCCESS != loader_get_data_files(inst, LOADER_DATA_FILE_MANIFEST_LAYER, true, VK_LAYER_PATH_ENV_VAR, override_paths,
                                                VK_ELAYERS_INFO_REGISTRY_LOC, VK_ELAYERS_INFO_RELATIVE_DIR, &manifest_files)) {
            goto out;
        }

        for (uint32_t i = 0; i < manifest_files.count; i++) {
            file_str = manifest_files.filename_list[i];
            if (file_str == NULL) {
                continue;
            }

            // parse file into JSON struct
            res = loader_get_json(inst, file_str, &json);
            if (VK_ERROR_OUT_OF_HOST_MEMORY == res) {
                goto out;
            } else if (VK_SUCCESS != res || NULL == json) {
                continue;
            }

            res = loader_add_layer_properties(inst, instance_layers, json, false, file_str);

            loader_instance_heap_free(inst, file_str);
            manifest_files.filename_list[i] = NULL;
            cJSON_Delete(inst, json);

            if (VK_ERROR_OUT_OF_HOST_MEMORY == res) {
                goto out;
            }
        }
    }

    // Verify any meta-layers in the list are valid and all the component layers are
    // actually present in the available layer list
    verify_all_meta_layers(inst, instance_layers, &override_layer_valid);

    if (override_layer_valid || implicit_metalayer_present) {
        loader_remove_layers_not_in_implicit_meta_layers(inst, instance_layers);
        if (override_layer_valid && inst != NULL) {
            inst->override_layer_present = true;
        }
    }

out:

    if (NULL != override_paths) {
        loader_instance_heap_free(inst, override_paths);
    }
    for (uint32_t i = 0; i < manifest_files.count; i++) {
        if (NULL != manifest_files.filename_list[i]) {
            loader_instance_heap_free(inst, manifest_files.filename_list[i]);
        }
    }
    if (NULL != manifest_files.filename_list) {
        loader_instance_heap_free(inst, manifest_files.filename_list);
    }

    if (have_json_lock) {
        loader_platform_thread_unlock_mutex(&loader_json_lock);
    }
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL loader_gpdpa_instance_internal(VkInstance inst, const char *pName) {
    // inst is not wrapped
    if (inst == VK_NULL_HANDLE) {
        return NULL;
    }
    VkLayerInstanceDispatchTable *disp_table = *(VkLayerInstanceDispatchTable **)inst;
    void *addr;

    if (disp_table == NULL) return NULL;

    bool found_name;
    addr = loader_lookup_instance_dispatch_table(disp_table, pName, &found_name);
    if (found_name) {
        return addr;
    }

    if (loader_phys_dev_ext_gpa(loader_get_instance(inst), pName, true, NULL, &addr)) return addr;

    // Don't call down the chain, this would be an infinite loop
    loader_log(NULL, VULKAN_LOADER_DEBUG_BIT, 0, "loader_gpdpa_instance_internal() unrecognized name %s", pName);
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL loader_gpdpa_instance_terminator(VkInstance inst, const char *pName) {
    // inst is not wrapped
    if (inst == VK_NULL_HANDLE) {
        return NULL;
    }
    VkLayerInstanceDispatchTable *disp_table = *(VkLayerInstanceDispatchTable **)inst;
    void *addr;

    if (disp_table == NULL) return NULL;

    bool found_name;
    addr = loader_lookup_instance_dispatch_table(disp_table, pName, &found_name);
    if (found_name) {
        return addr;
    }

    // Get the terminator, but don't perform checking since it should already
    // have been setup if we get here.
    if (loader_phys_dev_ext_gpa(loader_get_instance(inst), pName, false, NULL, &addr)) {
        return addr;
    }

    // Don't call down the chain, this would be an infinite loop
    loader_log(NULL, VULKAN_LOADER_DEBUG_BIT, 0, "loader_gpdpa_instance_terminator() unrecognized name %s", pName);
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL loader_gpa_instance_internal(VkInstance inst, const char *pName) {
    if (!strcmp(pName, "vkGetInstanceProcAddr")) {
        return (PFN_vkVoidFunction)loader_gpa_instance_internal;
    }
    if (!strcmp(pName, "vk_layerGetPhysicalDeviceProcAddr")) {
        return (PFN_vkVoidFunction)loader_gpdpa_instance_terminator;
    }
    if (!strcmp(pName, "vkCreateInstance")) {
        return (PFN_vkVoidFunction)terminator_CreateInstance;
    }
    if (!strcmp(pName, "vkCreateDevice")) {
        return (PFN_vkVoidFunction)terminator_CreateDevice;
    }

    // The VK_EXT_debug_utils functions need a special case here so the terminators can still be found from vkGetInstanceProcAddr
    if (!strcmp(pName, "vkSetDebugUtilsObjectNameEXT")) {
        return (PFN_vkVoidFunction)terminator_SetDebugUtilsObjectNameEXT;
    }
    if (!strcmp(pName, "vkSetDebugUtilsObjectTagEXT")) {
        return (PFN_vkVoidFunction)terminator_SetDebugUtilsObjectTagEXT;
    }
    if (!strcmp(pName, "vkQueueBeginDebugUtilsLabelEXT")) {
        return (PFN_vkVoidFunction)terminator_QueueBeginDebugUtilsLabelEXT;
    }
    if (!strcmp(pName, "vkQueueEndDebugUtilsLabelEXT")) {
        return (PFN_vkVoidFunction)terminator_QueueEndDebugUtilsLabelEXT;
    }
    if (!strcmp(pName, "vkQueueInsertDebugUtilsLabelEXT")) {
        return (PFN_vkVoidFunction)terminator_QueueInsertDebugUtilsLabelEXT;
    }
    if (!strcmp(pName, "vkCmdBeginDebugUtilsLabelEXT")) {
        return (PFN_vkVoidFunction)terminator_CmdBeginDebugUtilsLabelEXT;
    }
    if (!strcmp(pName, "vkCmdEndDebugUtilsLabelEXT")) {
        return (PFN_vkVoidFunction)terminator_CmdEndDebugUtilsLabelEXT;
    }
    if (!strcmp(pName, "vkCmdInsertDebugUtilsLabelEXT")) {
        return (PFN_vkVoidFunction)terminator_CmdInsertDebugUtilsLabelEXT;
    }

    // inst is not wrapped
    if (inst == VK_NULL_HANDLE) {
        return NULL;
    }
    VkLayerInstanceDispatchTable *disp_table = *(VkLayerInstanceDispatchTable **)inst;
    void *addr;

    if (disp_table == NULL) return NULL;

    bool found_name;
    addr = loader_lookup_instance_dispatch_table(disp_table, pName, &found_name);
    if (found_name) {
        return addr;
    }

    // Don't call down the chain, this would be an infinite loop
    loader_log(NULL, VULKAN_LOADER_DEBUG_BIT, 0, "loader_gpa_instance_internal() unrecognized name %s", pName);
    return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL loader_gpa_device_internal(VkDevice device, const char *pName) {
    struct loader_device *dev;
    struct loader_icd_term *icd_term = loader_get_icd_and_device(device, &dev, NULL);

    // Return this function if a layer above here is asking for the vkGetDeviceProcAddr.
    // This is so we can properly intercept any device commands needing a terminator.
    if (!strcmp(pName, "vkGetDeviceProcAddr")) {
        return (PFN_vkVoidFunction)loader_gpa_device_internal;
    }

    // NOTE: Device Funcs needing Trampoline/Terminator.
    // Overrides for device functions needing a trampoline and
    // a terminator because certain device entry-points still need to go
    // through a terminator before hitting the ICD.  This could be for
    // several reasons, but the main one is currently unwrapping an
    // object before passing the appropriate info along to the ICD.
    // This is why we also have to override the direct ICD call to
    // vkGetDeviceProcAddr to intercept those calls.
    PFN_vkVoidFunction addr = get_extension_device_proc_terminator(dev, pName);
    if (NULL != addr) {
        return addr;
    }

    return icd_term->dispatch.GetDeviceProcAddr(device, pName);
}

// Initialize device_ext dispatch table entry as follows:
// If dev == NULL find all logical devices created within this instance and
//  init the entry (given by idx) in the ext dispatch table.
// If dev != NULL only initialize the entry in the given dev's dispatch table.
// The initialization value is gotten by calling down the device chain with
// GDPA.
// If GDPA returns NULL then don't initialize the dispatch table entry.
static void loader_init_dispatch_dev_ext_entry(struct loader_instance *inst, struct loader_device *dev, uint32_t idx,
                                               const char *funcName)

{
    void *gdpa_value;
    if (dev != NULL) {
        gdpa_value = dev->loader_dispatch.core_dispatch.GetDeviceProcAddr(dev->chain_device, funcName);
        if (gdpa_value != NULL) dev->loader_dispatch.ext_dispatch.dev_ext[idx] = (PFN_vkDevExt)gdpa_value;
    } else {
        for (struct loader_icd_term *icd_term = inst->icd_terms; icd_term != NULL; icd_term = icd_term->next) {
            struct loader_device *ldev = icd_term->logical_device_list;
            while (ldev) {
                gdpa_value = ldev->loader_dispatch.core_dispatch.GetDeviceProcAddr(ldev->chain_device, funcName);
                if (gdpa_value != NULL) ldev->loader_dispatch.ext_dispatch.dev_ext[idx] = (PFN_vkDevExt)gdpa_value;
                ldev = ldev->next;
            }
        }
    }
}

// Find all dev extension in the hash table  and initialize the dispatch table
// for dev  for each of those extension entrypoints found in hash table.
void loader_init_dispatch_dev_ext(struct loader_instance *inst, struct loader_device *dev) {
    for (uint32_t i = 0; i < MAX_NUM_UNKNOWN_EXTS; i++) {
        if (inst->dev_ext_disp_hash[i].func_name != NULL)
            loader_init_dispatch_dev_ext_entry(inst, dev, i, inst->dev_ext_disp_hash[i].func_name);
    }
}

static bool loader_check_icds_for_dev_ext_address(struct loader_instance *inst, const char *funcName) {
    struct loader_icd_term *icd_term;
    icd_term = inst->icd_terms;
    while (NULL != icd_term) {
        if (icd_term->scanned_icd->GetInstanceProcAddr(icd_term->instance, funcName))
            // this icd supports funcName
            return true;
        icd_term = icd_term->next;
    }

    return false;
}

static bool loader_check_layer_list_for_dev_ext_address(const struct loader_layer_list *const layers, const char *funcName) {
    // Iterate over the layers.
    for (uint32_t layer = 0; layer < layers->count; ++layer) {
        // Iterate over the extensions.
        const struct loader_device_extension_list *const extensions = &(layers->list[layer].device_extension_list);
        for (uint32_t extension = 0; extension < extensions->count; ++extension) {
            // Iterate over the entry points.
            const struct loader_dev_ext_props *const property = &(extensions->list[extension]);
            for (uint32_t entry = 0; entry < property->entrypoint_count; ++entry) {
                if (strcmp(property->entrypoints[entry], funcName) == 0) {
                    return true;
                }
            }
        }
    }

    return false;
}

static void loader_free_dev_ext_table(struct loader_instance *inst) {
    for (uint32_t i = 0; i < MAX_NUM_UNKNOWN_EXTS; i++) {
        loader_instance_heap_free(inst, inst->dev_ext_disp_hash[i].func_name);
        loader_instance_heap_free(inst, inst->dev_ext_disp_hash[i].list.index);
    }
    memset(inst->dev_ext_disp_hash, 0, sizeof(inst->dev_ext_disp_hash));
}

static bool loader_add_dev_ext_table(struct loader_instance *inst, uint32_t *ptr_idx, const char *funcName) {
    uint32_t i;
    uint32_t idx = *ptr_idx;
    struct loader_dispatch_hash_list *list = &inst->dev_ext_disp_hash[idx].list;

    if (!inst->dev_ext_disp_hash[idx].func_name) {
        // no entry here at this idx, so use it
        assert(list->capacity == 0);
        inst->dev_ext_disp_hash[idx].func_name =
            (char *)loader_instance_heap_alloc(inst, strlen(funcName) + 1, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
        if (inst->dev_ext_disp_hash[idx].func_name == NULL) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_add_dev_ext_table: Failed to allocate memory for func_name %s",
                       funcName);
            return false;
        }
        strncpy(inst->dev_ext_disp_hash[idx].func_name, funcName, strlen(funcName) + 1);
        return true;
    }

    // check for enough capacity
    if (list->capacity == 0) {
        list->index = loader_instance_heap_alloc(inst, 8 * sizeof(*(list->index)), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
        if (list->index == NULL) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_add_dev_ext_table: Failed to allocate memory for list index of function %s", funcName);
            return false;
        }
        list->capacity = 8 * sizeof(*(list->index));
    } else if (list->capacity < (list->count + 1) * sizeof(*(list->index))) {
        void *new_ptr = loader_instance_heap_realloc(inst, list->index, list->capacity, list->capacity * 2,
                                                     VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
        if (NULL == new_ptr) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_add_dev_ext_table: Failed to reallocate memory for list index of function %s", funcName);
            return false;
        }
        list->index = new_ptr;
        list->capacity *= 2;
    }

    // find an unused index in the hash table and use it
    i = (idx + 1) % MAX_NUM_UNKNOWN_EXTS;
    do {
        if (!inst->dev_ext_disp_hash[i].func_name) {
            assert(inst->dev_ext_disp_hash[i].list.capacity == 0);
            inst->dev_ext_disp_hash[i].func_name =
                (char *)loader_instance_heap_alloc(inst, strlen(funcName) + 1, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
            if (inst->dev_ext_disp_hash[i].func_name == NULL) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_add_dev_ext_table: Failed to allocate memory for func_name %s",
                           funcName);
                return false;
            }
            strncpy(inst->dev_ext_disp_hash[i].func_name, funcName, strlen(funcName) + 1);
            list->index[list->count] = i;
            list->count++;
            *ptr_idx = i;
            return true;
        }
        i = (i + 1) % MAX_NUM_UNKNOWN_EXTS;
    } while (i != idx);

    loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_add_dev_ext_table:  Could not insert into hash table; is it full?");

    return false;
}

static bool loader_name_in_dev_ext_table(struct loader_instance *inst, uint32_t *idx, const char *funcName) {
    uint32_t alt_idx;
    if (inst->dev_ext_disp_hash[*idx].func_name && !strcmp(inst->dev_ext_disp_hash[*idx].func_name, funcName)) return true;

    // funcName wasn't at the primary spot in the hash table
    // search the list of secondary locations (shallow search, not deep search)
    for (uint32_t i = 0; i < inst->dev_ext_disp_hash[*idx].list.count; i++) {
        alt_idx = inst->dev_ext_disp_hash[*idx].list.index[i];
        if (inst->dev_ext_disp_hash[*idx].func_name && !strcmp(inst->dev_ext_disp_hash[*idx].func_name, funcName)) {
            *idx = alt_idx;
            return true;
        }
    }

    return false;
}

// This function returns generic trampoline code address for unknown entry
// points.
// Presumably, these unknown entry points (as given by funcName) are device
// extension entrypoints.  A hash table is used to keep a list of unknown entry
// points and their mapping to the device extension dispatch table
// (struct loader_dev_ext_dispatch_table).
// \returns
// For a given entry point string (funcName), if an existing mapping is found
// the
// trampoline address for that mapping is returned. Otherwise, this unknown
// entry point
// has not been seen yet. Next check if a layer or ICD supports it.  If so then
// a
// new entry in the hash table is initialized and that trampoline address for
// the new entry is returned. Null is returned if the hash table is full or
// if no discovered layer or ICD returns a non-NULL GetProcAddr for it.
void *loader_dev_ext_gpa(struct loader_instance *inst, const char *funcName) {
    uint32_t idx;
    uint32_t seed = 0;

    idx = murmurhash(funcName, strlen(funcName), seed) % MAX_NUM_UNKNOWN_EXTS;

    if (loader_name_in_dev_ext_table(inst, &idx, funcName))
        // found funcName already in hash
        return loader_get_dev_ext_trampoline(idx);

    // Check if funcName is supported in either ICDs or a layer library
    if (!loader_check_icds_for_dev_ext_address(inst, funcName) &&
        !loader_check_layer_list_for_dev_ext_address(&inst->app_activated_layer_list, funcName)) {
        // if support found in layers continue on
        return NULL;
    }

    if (loader_add_dev_ext_table(inst, &idx, funcName)) {
        // successfully added new table entry
        // init any dev dispatch table entries as needed
        loader_init_dispatch_dev_ext_entry(inst, NULL, idx, funcName);
        return loader_get_dev_ext_trampoline(idx);
    }

    return NULL;
}

static bool loader_check_icds_for_phys_dev_ext_address(struct loader_instance *inst, const char *funcName) {
    struct loader_icd_term *icd_term;
    icd_term = inst->icd_terms;
    while (NULL != icd_term) {
        if (icd_term->scanned_icd->interface_version >= MIN_PHYS_DEV_EXTENSION_ICD_INTERFACE_VERSION &&
            icd_term->scanned_icd->GetPhysicalDeviceProcAddr(icd_term->instance, funcName))
            // this icd supports funcName
            return true;
        icd_term = icd_term->next;
    }

    return false;
}

static bool loader_check_layer_list_for_phys_dev_ext_address(struct loader_instance *inst, const char *funcName) {
    struct loader_layer_properties *layer_prop_list = inst->expanded_activated_layer_list.list;
    for (uint32_t layer = 0; layer < inst->expanded_activated_layer_list.count; ++layer) {
        // If this layer supports the vk_layerGetPhysicalDeviceProcAddr, then call
        // it and see if it returns a valid pointer for this function name.
        if (layer_prop_list[layer].interface_version > 1) {
            const struct loader_layer_functions *const functions = &(layer_prop_list[layer].functions);
            if (NULL != functions->get_physical_device_proc_addr &&
                NULL != functions->get_physical_device_proc_addr((VkInstance)inst->instance, funcName)) {
                return true;
            }
        }
    }

    return false;
}

static void loader_free_phys_dev_ext_table(struct loader_instance *inst) {
    for (uint32_t i = 0; i < MAX_NUM_UNKNOWN_EXTS; i++) {
        loader_instance_heap_free(inst, inst->phys_dev_ext_disp_hash[i].func_name);
        loader_instance_heap_free(inst, inst->phys_dev_ext_disp_hash[i].list.index);
    }
    memset(inst->phys_dev_ext_disp_hash, 0, sizeof(inst->phys_dev_ext_disp_hash));
}

static bool loader_add_phys_dev_ext_table(struct loader_instance *inst, uint32_t *ptr_idx, const char *funcName) {
    uint32_t i;
    uint32_t idx = *ptr_idx;
    struct loader_dispatch_hash_list *list = &inst->phys_dev_ext_disp_hash[idx].list;

    if (!inst->phys_dev_ext_disp_hash[idx].func_name) {
        // no entry here at this idx, so use it
        assert(list->capacity == 0);
        inst->phys_dev_ext_disp_hash[idx].func_name =
            (char *)loader_instance_heap_alloc(inst, strlen(funcName) + 1, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
        if (inst->phys_dev_ext_disp_hash[idx].func_name == NULL) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_add_phys_dev_ext_table() can't allocate memory for func_name");
            return false;
        }
        strncpy(inst->phys_dev_ext_disp_hash[idx].func_name, funcName, strlen(funcName) + 1);
        return true;
    }

    // check for enough capacity
    if (list->capacity == 0) {
        list->index = loader_instance_heap_alloc(inst, 8 * sizeof(*(list->index)), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
        if (list->index == NULL) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_add_phys_dev_ext_table() can't allocate list memory");
            return false;
        }
        list->capacity = 8 * sizeof(*(list->index));
    } else if (list->capacity < (list->count + 1) * sizeof(*(list->index))) {
        void *new_ptr = loader_instance_heap_realloc(inst, list->index, list->capacity, list->capacity * 2,
                                                     VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
        if (NULL == new_ptr) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_add_phys_dev_ext_table() can't reallocate list memory");
            return false;
        }
        list->index = new_ptr;
        list->capacity *= 2;
    }

    // find an unused index in the hash table and use it
    i = (idx + 1) % MAX_NUM_UNKNOWN_EXTS;
    do {
        if (!inst->phys_dev_ext_disp_hash[i].func_name) {
            assert(inst->phys_dev_ext_disp_hash[i].list.capacity == 0);
            inst->phys_dev_ext_disp_hash[i].func_name =
                (char *)loader_instance_heap_alloc(inst, strlen(funcName) + 1, VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
            if (inst->phys_dev_ext_disp_hash[i].func_name == NULL) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_add_dev_ext_table() can't reallocate func_name memory");
                return false;
            }
            strncpy(inst->phys_dev_ext_disp_hash[i].func_name, funcName, strlen(funcName) + 1);
            list->index[list->count] = i;
            list->count++;
            *ptr_idx = i;
            return true;
        }
        i = (i + 1) % MAX_NUM_UNKNOWN_EXTS;
    } while (i != idx);

    loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_add_phys_dev_ext_table() couldn't insert into hash table; is it full?");
    return false;
}

static bool loader_name_in_phys_dev_ext_table(struct loader_instance *inst, uint32_t *idx, const char *funcName) {
    uint32_t alt_idx;
    if (inst->phys_dev_ext_disp_hash[*idx].func_name && !strcmp(inst->phys_dev_ext_disp_hash[*idx].func_name, funcName))
        return true;

    // funcName wasn't at the primary spot in the hash table
    // search the list of secondary locations (shallow search, not deep search)
    for (uint32_t i = 0; i < inst->phys_dev_ext_disp_hash[*idx].list.count; i++) {
        alt_idx = inst->phys_dev_ext_disp_hash[*idx].list.index[i];
        if (inst->phys_dev_ext_disp_hash[*idx].func_name && !strcmp(inst->phys_dev_ext_disp_hash[*idx].func_name, funcName)) {
            *idx = alt_idx;
            return true;
        }
    }

    return false;
}

// This function returns a generic trampoline and/or terminator function
// address for any unknown physical device extension commands.  A hash
// table is used to keep a list of unknown entry points and their
// mapping to the physical device extension dispatch table (struct
// loader_phys_dev_ext_dispatch_table).
// For a given entry point string (funcName), if an existing mapping is
// found, then the trampoline address for that mapping is returned in
// tramp_addr (if it is not NULL) and the terminator address for that
// mapping is returned in term_addr (if it is not NULL). Otherwise,
// this unknown entry point has not been seen yet.
// If it has not been seen before, and perform_checking is 'true',
// check if a layer or and ICD supports it.  If so then a new entry in
// the hash table is initialized and the trampoline and/or terminator
// addresses are returned.
// Null is returned if the hash table is full or if no discovered layer or
// ICD returns a non-NULL GetProcAddr for it.
bool loader_phys_dev_ext_gpa(struct loader_instance *inst, const char *funcName, bool perform_checking, void **tramp_addr,
                             void **term_addr) {
    uint32_t idx;
    uint32_t seed = 0;
    bool success = false;

    if (inst == NULL) {
        goto out;
    }

    if (NULL != tramp_addr) {
        *tramp_addr = NULL;
    }
    if (NULL != term_addr) {
        *term_addr = NULL;
    }

    // We should always check to see if any ICD supports it.
    if (!loader_check_icds_for_phys_dev_ext_address(inst, funcName)) {
        // If we're not checking layers, or we are and it's not in a layer, just
        // return
        if (!perform_checking || !loader_check_layer_list_for_phys_dev_ext_address(inst, funcName)) {
            goto out;
        }
    }

    idx = murmurhash(funcName, strlen(funcName), seed) % MAX_NUM_UNKNOWN_EXTS;
    if (perform_checking && !loader_name_in_phys_dev_ext_table(inst, &idx, funcName)) {
        uint32_t i;

        // Only need to add first one to get index in Instance.  Others will use
        // the same index.
        if (!loader_add_phys_dev_ext_table(inst, &idx, funcName)) {
            // couldn't perform the above function due to insufficient memory available
            goto out;
        }

        // Setup the ICD function pointers
        struct loader_icd_term *icd_term = inst->icd_terms;
        while (NULL != icd_term) {
            if (MIN_PHYS_DEV_EXTENSION_ICD_INTERFACE_VERSION <= icd_term->scanned_icd->interface_version &&
                NULL != icd_term->scanned_icd->GetPhysicalDeviceProcAddr) {
                icd_term->phys_dev_ext[idx] =
                    (PFN_PhysDevExt)icd_term->scanned_icd->GetPhysicalDeviceProcAddr(icd_term->instance, funcName);

                // Make sure we set the instance dispatch to point to the
                // loader's terminator now since we can at least handle it
                // in one ICD.
                inst->disp->phys_dev_ext[idx] = loader_get_phys_dev_ext_termin(idx);
            } else {
                icd_term->phys_dev_ext[idx] = NULL;
            }

            icd_term = icd_term->next;
        }

        // Now, search for the first layer attached and query using it to get
        // the first entry point.
        for (i = 0; i < inst->expanded_activated_layer_list.count; i++) {
            struct loader_layer_properties *layer_prop = &inst->expanded_activated_layer_list.list[i];
            if (layer_prop->interface_version > 1 && NULL != layer_prop->functions.get_physical_device_proc_addr) {
                inst->disp->phys_dev_ext[idx] =
                    (PFN_PhysDevExt)layer_prop->functions.get_physical_device_proc_addr((VkInstance)inst->instance, funcName);
                if (NULL != inst->disp->phys_dev_ext[idx]) {
                    break;
                }
            }
        }
    }

    if (NULL != tramp_addr) {
        *tramp_addr = loader_get_phys_dev_ext_tramp(idx);
    }

    if (NULL != term_addr) {
        *term_addr = loader_get_phys_dev_ext_termin(idx);
    }

    success = true;

out:
    return success;
}

struct loader_instance *loader_get_instance(const VkInstance instance) {
    // look up the loader_instance in our list by comparing dispatch tables, as
    // there is no guarantee the instance is still a loader_instance* after any
    // layers which wrap the instance object.
    const VkLayerInstanceDispatchTable *disp;
    struct loader_instance *ptr_instance = (struct loader_instance *)instance;
    if (VK_NULL_HANDLE == instance || LOADER_MAGIC_NUMBER != ptr_instance->magic) {
        return NULL;
    } else {
        disp = loader_get_instance_layer_dispatch(instance);
        for (struct loader_instance *inst = loader.instances; inst; inst = inst->next) {
            if (&inst->disp->layer_inst_disp == disp) {
                ptr_instance = inst;
                break;
            }
        }
    }
    return ptr_instance;
}

static loader_platform_dl_handle loader_open_layer_file(const struct loader_instance *inst, const char *chain_type,
                                                        struct loader_layer_properties *prop) {
    if ((prop->lib_handle = loader_platform_open_library(prop->lib_name)) == NULL) {
        loader_log_load_library_error(inst, prop->lib_name);
    } else {
        loader_log(inst, VULKAN_LOADER_DEBUG_BIT | VULKAN_LOADER_LAYER_BIT, 0, "Loading layer library %s", prop->lib_name);
    }

    return prop->lib_handle;
}

static void loader_close_layer_file(const struct loader_instance *inst, struct loader_layer_properties *prop) {
    if (prop->lib_handle) {
        loader_platform_close_library(prop->lib_handle);
        loader_log(inst, VULKAN_LOADER_DEBUG_BIT | VULKAN_LOADER_LAYER_BIT, 0, "Unloading layer library %s", prop->lib_name);
        prop->lib_handle = NULL;
    }
}

void loader_deactivate_layers(const struct loader_instance *instance, struct loader_device *device,
                              struct loader_layer_list *list) {
    // Delete instance list of enabled layers and close any layer libraries
    for (uint32_t i = 0; i < list->count; i++) {
        struct loader_layer_properties *layer_prop = &list->list[i];

        loader_close_layer_file(instance, layer_prop);
    }
    loader_destroy_layer_list(instance, device, list);
}

// Go through the search_list and find any layers which match type. If layer
// type match is found in then add it to ext_list.
static void loader_add_implicit_layers(const struct loader_instance *inst, struct loader_layer_list *target_list,
                                       struct loader_layer_list *expanded_target_list,
                                       const struct loader_layer_list *source_list) {
    for (uint32_t src_layer = 0; src_layer < source_list->count; src_layer++) {
        const struct loader_layer_properties *prop = &source_list->list[src_layer];
        if (0 == (prop->type_flags & VK_LAYER_TYPE_FLAG_EXPLICIT_LAYER)) {
            loader_add_implicit_layer(inst, prop, target_list, expanded_target_list, source_list);
        }
    }
}

// Get the layer name(s) from the env_name environment variable. If layer is found in
// search_list then add it to layer_list.  But only add it to layer_list if type_flags matches.
static VkResult loader_add_environment_layers(struct loader_instance *inst, const enum layer_type_flags type_flags,
                                              const char *env_name, struct loader_layer_list *target_list,
                                              struct loader_layer_list *expanded_target_list,
                                              const struct loader_layer_list *source_list) {
    VkResult res = VK_SUCCESS;
    char *next, *name;
    char *layer_env = loader_getenv(env_name, inst);
    if (layer_env == NULL) {
        goto out;
    }
    name = loader_stack_alloc(strlen(layer_env) + 1);
    if (name == NULL) {
        goto out;
    }
    strcpy(name, layer_env);

    while (name && *name) {
        next = loader_get_next_path(name);
        res = loader_add_layer_name_to_list(inst, name, type_flags, source_list, target_list, expanded_target_list);
        if (res != VK_SUCCESS) {
            goto out;
        }
        name = next;
    }

out:

    if (layer_env != NULL) {
        loader_free_getenv(layer_env, inst);
    }

    return res;
}

VkResult loader_enable_instance_layers(struct loader_instance *inst, const VkInstanceCreateInfo *pCreateInfo,
                                       const struct loader_layer_list *instance_layers) {
    VkResult err = VK_SUCCESS;
    uint16_t layer_api_major_version;
    uint16_t layer_api_minor_version;
    uint32_t i;
    struct loader_layer_properties *prop;

    assert(inst && "Cannot have null instance");

    if (!loader_init_layer_list(inst, &inst->app_activated_layer_list)) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "loader_enable_instance_layers: Failed to initialize application version of the layer list");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    if (!loader_init_layer_list(inst, &inst->expanded_activated_layer_list)) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "loader_enable_instance_layers: Failed to initialize expanded version of the layer list");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // Add any implicit layers first
    loader_add_implicit_layers(inst, &inst->app_activated_layer_list, &inst->expanded_activated_layer_list, instance_layers);

    // Add any layers specified via environment variable next
    err = loader_add_environment_layers(inst, VK_LAYER_TYPE_FLAG_EXPLICIT_LAYER, "VK_INSTANCE_LAYERS",
                                        &inst->app_activated_layer_list, &inst->expanded_activated_layer_list, instance_layers);
    if (err != VK_SUCCESS) {
        goto out;
    }

    // Add layers specified by the application
    err = loader_add_layer_names_to_list(inst, &inst->app_activated_layer_list, &inst->expanded_activated_layer_list,
                                         pCreateInfo->enabledLayerCount, pCreateInfo->ppEnabledLayerNames, instance_layers);

    for (i = 0; i < inst->expanded_activated_layer_list.count; i++) {
        // Verify that the layer api version is at least that of the application's request, if not, throw a warning since
        // undefined behavior could occur.
        prop = inst->expanded_activated_layer_list.list + i;
        layer_api_major_version = VK_VERSION_MAJOR(prop->info.specVersion);
        layer_api_minor_version = VK_VERSION_MINOR(prop->info.specVersion);
        if (inst->app_api_major_version > layer_api_major_version ||
            (inst->app_api_major_version == layer_api_major_version && inst->app_api_minor_version > layer_api_minor_version)) {
            loader_log(inst, VULKAN_LOADER_WARN_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                       "loader_add_to_layer_list: Explicit layer %s is using an old API version %" PRIu16 ".%" PRIu16
                       " versus application requested %" PRIu16 ".%" PRIu16,
                       prop->info.layerName, layer_api_major_version, layer_api_minor_version, inst->app_api_major_version,
                       inst->app_api_minor_version);
        }
    }

out:
    return err;
}

// Determine the layer interface version to use.
bool loader_get_layer_interface_version(PFN_vkNegotiateLoaderLayerInterfaceVersion fp_negotiate_layer_version,
                                        VkNegotiateLayerInterface *interface_struct) {
    memset(interface_struct, 0, sizeof(VkNegotiateLayerInterface));
    interface_struct->sType = LAYER_NEGOTIATE_INTERFACE_STRUCT;
    interface_struct->loaderLayerInterfaceVersion = 1;
    interface_struct->pNext = NULL;

    if (fp_negotiate_layer_version != NULL) {
        // Layer supports the negotiation API, so call it with the loader's
        // latest version supported
        interface_struct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
        VkResult result = fp_negotiate_layer_version(interface_struct);

        if (result != VK_SUCCESS) {
            // Layer no longer supports the loader's latest interface version so
            // fail loading the Layer
            return false;
        }
    }

    if (interface_struct->loaderLayerInterfaceVersion < MIN_SUPPORTED_LOADER_LAYER_INTERFACE_VERSION) {
        // Loader no longer supports the layer's latest interface version so
        // fail loading the layer
        return false;
    }

    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL loader_layer_create_device(VkInstance instance, VkPhysicalDevice physicalDevice,
                                                          const VkDeviceCreateInfo *pCreateInfo,
                                                          const VkAllocationCallbacks *pAllocator, VkDevice *pDevice,
                                                          PFN_vkGetInstanceProcAddr layerGIPA, PFN_vkGetDeviceProcAddr *nextGDPA) {
    VkResult res;
    VkPhysicalDevice internal_device = VK_NULL_HANDLE;
    struct loader_device *dev = NULL;
    struct loader_instance *inst = NULL;

    if (instance != VK_NULL_HANDLE) {
        inst = loader_get_instance(instance);
        internal_device = physicalDevice;
    } else {
        struct loader_physical_device_tramp *phys_dev = (struct loader_physical_device_tramp *)physicalDevice;
        internal_device = phys_dev->phys_dev;
        inst = (struct loader_instance *)phys_dev->this_instance;
    }

    // Get the physical device (ICD) extensions
    struct loader_extension_list icd_exts;
    icd_exts.list = NULL;
    res = loader_init_generic_list(inst, (struct loader_generic_list *)&icd_exts, sizeof(VkExtensionProperties));
    if (VK_SUCCESS != res) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "vkCreateDevice: Failed to create ICD extension list");
        goto out;
    }

    PFN_vkEnumerateDeviceExtensionProperties enumDeviceExtensionProperties = NULL;
    if (layerGIPA != NULL) {
        enumDeviceExtensionProperties =
            (PFN_vkEnumerateDeviceExtensionProperties)layerGIPA(instance, "vkEnumerateDeviceExtensionProperties");
    } else {
        enumDeviceExtensionProperties = inst->disp->layer_inst_disp.EnumerateDeviceExtensionProperties;
    }
    res = loader_add_device_extensions(inst, enumDeviceExtensionProperties, internal_device, "Unknown", &icd_exts);
    if (res != VK_SUCCESS) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "vkCreateDevice: Failed to add extensions to list");
        goto out;
    }

    // Make sure requested extensions to be enabled are supported
    res = loader_validate_device_extensions(inst, &inst->expanded_activated_layer_list, &icd_exts, pCreateInfo);
    if (res != VK_SUCCESS) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "vkCreateDevice: Failed to validate extensions in list");
        goto out;
    }

    dev = loader_create_logical_device(inst, pAllocator);
    if (dev == NULL) {
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }

    // Copy the application enabled instance layer list into the device
    if (NULL != inst->app_activated_layer_list.list) {
        dev->app_activated_layer_list.capacity = inst->app_activated_layer_list.capacity;
        dev->app_activated_layer_list.count = inst->app_activated_layer_list.count;
        dev->app_activated_layer_list.list =
            loader_device_heap_alloc(dev, inst->app_activated_layer_list.capacity, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
        if (dev->app_activated_layer_list.list == NULL) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "vkCreateDevice: Failed to allocate application activated layer list of size %d.",
                       inst->app_activated_layer_list.capacity);
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto out;
        }
        memcpy(dev->app_activated_layer_list.list, inst->app_activated_layer_list.list,
               sizeof(*dev->app_activated_layer_list.list) * dev->app_activated_layer_list.count);
    } else {
        dev->app_activated_layer_list.capacity = 0;
        dev->app_activated_layer_list.count = 0;
        dev->app_activated_layer_list.list = NULL;
    }

    // Copy the expanded enabled instance layer list into the device
    if (NULL != inst->expanded_activated_layer_list.list) {
        dev->expanded_activated_layer_list.capacity = inst->expanded_activated_layer_list.capacity;
        dev->expanded_activated_layer_list.count = inst->expanded_activated_layer_list.count;
        dev->expanded_activated_layer_list.list =
            loader_device_heap_alloc(dev, inst->expanded_activated_layer_list.capacity, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
        if (dev->expanded_activated_layer_list.list == NULL) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "vkCreateDevice: Failed to allocate expanded activated layer list of size %d.",
                       inst->expanded_activated_layer_list.capacity);
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto out;
        }
        memcpy(dev->expanded_activated_layer_list.list, inst->expanded_activated_layer_list.list,
               sizeof(*dev->expanded_activated_layer_list.list) * dev->expanded_activated_layer_list.count);
    } else {
        dev->expanded_activated_layer_list.capacity = 0;
        dev->expanded_activated_layer_list.count = 0;
        dev->expanded_activated_layer_list.list = NULL;
    }

    res = loader_create_device_chain(internal_device, pCreateInfo, pAllocator, inst, dev, layerGIPA, nextGDPA);
    if (res != VK_SUCCESS) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "vkCreateDevice:  Failed to create device chain.");
        goto out;
    }

    *pDevice = dev->chain_device;

    // Initialize any device extension dispatch entry's from the instance list
    loader_init_dispatch_dev_ext(inst, dev);

    // Initialize WSI device extensions as part of core dispatch since loader
    // has dedicated trampoline code for these
    loader_init_device_extension_dispatch_table(&dev->loader_dispatch, inst->disp->layer_inst_disp.GetInstanceProcAddr,
                                                dev->loader_dispatch.core_dispatch.GetDeviceProcAddr, inst->instance, *pDevice);

out:

    // Failure cleanup
    if (VK_SUCCESS != res) {
        if (NULL != dev) {
            loader_destroy_logical_device(inst, dev, pAllocator);
        }
    }

    if (NULL != icd_exts.list) {
        loader_destroy_generic_list(inst, (struct loader_generic_list *)&icd_exts);
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL loader_layer_destroy_device(VkDevice device, const VkAllocationCallbacks *pAllocator,
                                                       PFN_vkDestroyDevice destroyFunction) {
    struct loader_device *dev;

    if (device == VK_NULL_HANDLE) {
        return;
    }

    struct loader_icd_term *icd_term = loader_get_icd_and_device(device, &dev, NULL);
    const struct loader_instance *inst = icd_term->this_instance;

    destroyFunction(device, pAllocator);
    dev->chain_device = NULL;
    dev->icd_device = NULL;
    loader_remove_logical_device(inst, icd_term, dev, pAllocator);
}

// Given the list of layers to activate in the loader_instance
// structure. This function will add a VkLayerInstanceCreateInfo
// structure to the VkInstanceCreateInfo.pNext pointer.
// Each activated layer will have it's own VkLayerInstanceLink
// structure that tells the layer what Get*ProcAddr to call to
// get function pointers to the next layer down.
// Once the chain info has been created this function will
// execute the CreateInstance call chain. Each layer will
// then have an opportunity in it's CreateInstance function
// to setup it's dispatch table when the lower layer returns
// successfully.
// Each layer can wrap or not-wrap the returned VkInstance object
// as it sees fit.
// The instance chain is terminated by a loader function
// that will call CreateInstance on all available ICD's and
// cache those VkInstance objects for future use.
VkResult loader_create_instance_chain(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                                      struct loader_instance *inst, VkInstance *created_instance) {
    uint32_t num_activated_layers = 0;
    struct activated_layer_info *activated_layers = NULL;
    VkLayerInstanceCreateInfo chain_info;
    VkLayerInstanceLink *layer_instance_link_info = NULL;
    VkInstanceCreateInfo loader_create_info;
    VkResult res;

    PFN_vkGetInstanceProcAddr next_gipa = loader_gpa_instance_internal;
    PFN_vkGetInstanceProcAddr cur_gipa = loader_gpa_instance_internal;
    PFN_vkGetDeviceProcAddr cur_gdpa = loader_gpa_device_internal;
    PFN_GetPhysicalDeviceProcAddr next_gpdpa = loader_gpdpa_instance_internal;
    PFN_GetPhysicalDeviceProcAddr cur_gpdpa = loader_gpdpa_instance_internal;

    memcpy(&loader_create_info, pCreateInfo, sizeof(VkInstanceCreateInfo));

    if (inst->expanded_activated_layer_list.count > 0) {
        chain_info.u.pLayerInfo = NULL;
        chain_info.pNext = pCreateInfo->pNext;
        chain_info.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
        chain_info.function = VK_LAYER_LINK_INFO;
        loader_create_info.pNext = &chain_info;

        layer_instance_link_info = loader_stack_alloc(sizeof(VkLayerInstanceLink) * inst->expanded_activated_layer_list.count);
        if (!layer_instance_link_info) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_create_instance_chain: Failed to alloc Instance objects for layer");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        activated_layers = loader_stack_alloc(sizeof(struct activated_layer_info) * inst->expanded_activated_layer_list.count);
        if (!activated_layers) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_create_instance_chain: Failed to alloc activated layer storage array");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        // Create instance chain of enabled layers
        for (int32_t i = inst->expanded_activated_layer_list.count - 1; i >= 0; i--) {
            struct loader_layer_properties *layer_prop = &inst->expanded_activated_layer_list.list[i];
            loader_platform_dl_handle lib_handle;

            // Skip it if a Layer with the same name has been already successfully activated
            if (loader_names_array_has_layer_property(&layer_prop->info, num_activated_layers, activated_layers)) {
                continue;
            }

            lib_handle = loader_open_layer_file(inst, "instance", layer_prop);
            if (!lib_handle) {
                continue;
            }

            if (NULL == layer_prop->functions.negotiate_layer_interface) {
                PFN_vkNegotiateLoaderLayerInterfaceVersion negotiate_interface = NULL;
                bool functions_in_interface = false;
                if (strlen(layer_prop->functions.str_negotiate_interface) == 0) {
                    negotiate_interface = (PFN_vkNegotiateLoaderLayerInterfaceVersion)loader_platform_get_proc_address(
                        lib_handle, "vkNegotiateLoaderLayerInterfaceVersion");
                } else {
                    negotiate_interface = (PFN_vkNegotiateLoaderLayerInterfaceVersion)loader_platform_get_proc_address(
                        lib_handle, layer_prop->functions.str_negotiate_interface);
                }

                // If we can negotiate an interface version, then we can also
                // get everything we need from the one function call, so try
                // that first, and see if we can get all the function pointers
                // necessary from that one call.
                if (NULL != negotiate_interface) {
                    layer_prop->functions.negotiate_layer_interface = negotiate_interface;

                    VkNegotiateLayerInterface interface_struct;

                    if (loader_get_layer_interface_version(negotiate_interface, &interface_struct)) {
                        // Go ahead and set the properties version to the
                        // correct value.
                        layer_prop->interface_version = interface_struct.loaderLayerInterfaceVersion;

                        // If the interface is 2 or newer, we have access to the
                        // new GetPhysicalDeviceProcAddr function, so grab it,
                        // and the other necessary functions, from the
                        // structure.
                        if (interface_struct.loaderLayerInterfaceVersion > 1) {
                            cur_gipa = interface_struct.pfnGetInstanceProcAddr;
                            cur_gdpa = interface_struct.pfnGetDeviceProcAddr;
                            cur_gpdpa = interface_struct.pfnGetPhysicalDeviceProcAddr;
                            if (cur_gipa != NULL) {
                                // We've set the functions, so make sure we
                                // don't do the unnecessary calls later.
                                functions_in_interface = true;
                            }
                        }
                    }
                }

                if (!functions_in_interface) {
                    if ((cur_gipa = layer_prop->functions.get_instance_proc_addr) == NULL) {
                        if (strlen(layer_prop->functions.str_gipa) == 0) {
                            cur_gipa =
                                (PFN_vkGetInstanceProcAddr)loader_platform_get_proc_address(lib_handle, "vkGetInstanceProcAddr");
                            layer_prop->functions.get_instance_proc_addr = cur_gipa;
                        } else {
                            cur_gipa = (PFN_vkGetInstanceProcAddr)loader_platform_get_proc_address(lib_handle,
                                                                                                   layer_prop->functions.str_gipa);
                        }

                        if (NULL == cur_gipa) {
                            loader_log(inst, VULKAN_LOADER_ERROR_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                                       "loader_create_instance_chain: Failed to find \'vkGetInstanceProcAddr\' in layer %s",
                                       layer_prop->lib_name);
                            continue;
                        }
                    }
                }
            }

            layer_instance_link_info[num_activated_layers].pNext = chain_info.u.pLayerInfo;
            layer_instance_link_info[num_activated_layers].pfnNextGetInstanceProcAddr = next_gipa;
            layer_instance_link_info[num_activated_layers].pfnNextGetPhysicalDeviceProcAddr = next_gpdpa;
            next_gipa = cur_gipa;
            if (layer_prop->interface_version > 1 && cur_gpdpa != NULL) {
                layer_prop->functions.get_physical_device_proc_addr = cur_gpdpa;
                next_gpdpa = cur_gpdpa;
            }
            if (layer_prop->interface_version > 1 && cur_gipa != NULL) {
                layer_prop->functions.get_instance_proc_addr = cur_gipa;
            }
            if (layer_prop->interface_version > 1 && cur_gdpa != NULL) {
                layer_prop->functions.get_device_proc_addr = cur_gdpa;
            }

            chain_info.u.pLayerInfo = &layer_instance_link_info[num_activated_layers];

            activated_layers[num_activated_layers].name = layer_prop->info.layerName;
            activated_layers[num_activated_layers].manifest = layer_prop->manifest_file_name;
            activated_layers[num_activated_layers].library = layer_prop->lib_name;
            activated_layers[num_activated_layers].is_implicit = !(layer_prop->type_flags & VK_LAYER_TYPE_FLAG_EXPLICIT_LAYER);
            if (activated_layers[num_activated_layers].is_implicit) {
                activated_layers[num_activated_layers].disable_env = layer_prop->disable_env_var.name;
            }

            loader_log(inst, VULKAN_LOADER_INFO_BIT | VULKAN_LOADER_LAYER_BIT, 0, "Insert instance layer %s (%s)",
                       layer_prop->info.layerName, layer_prop->lib_name);

            num_activated_layers++;
        }
    }

    VkLoaderFeatureFlags feature_flags = 0;
#if defined(_WIN32)
    feature_flags = windows_initialize_dxgi();
#endif

    PFN_vkCreateInstance fpCreateInstance = (PFN_vkCreateInstance)next_gipa(*created_instance, "vkCreateInstance");
    if (fpCreateInstance) {
        const VkLayerInstanceCreateInfo instance_dispatch = {
            .sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
            .pNext = loader_create_info.pNext,
            .function = VK_LOADER_DATA_CALLBACK,
            .u =
                {
                    .pfnSetInstanceLoaderData = vkSetInstanceDispatch,
                },
        };
        const VkLayerInstanceCreateInfo device_callback = {
            .sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
            .pNext = &instance_dispatch,
            .function = VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK,
            .u =
                {
                    .layerDevice =
                        {
                            .pfnLayerCreateDevice = loader_layer_create_device,
                            .pfnLayerDestroyDevice = loader_layer_destroy_device,
                        },
                },
        };
        const VkLayerInstanceCreateInfo loader_features = {
            .sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
            .pNext = &device_callback,
            .function = VK_LOADER_FEATURES,
            .u =
                {
                    .loaderFeatures = feature_flags,
                },
        };
        loader_create_info.pNext = &loader_features;

        // If layer debugging is enabled, let's print out the full callstack with layers in their
        // defined order.
        if ((loader_get_debug_level() & VULKAN_LOADER_LAYER_BIT) != 0) {
            loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "vkCreateInstance layer callstack setup to:");
            loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "   <Application>");
            loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "     ||");
            loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "   <Loader>");
            loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "     ||");
            for (uint32_t cur_layer = 0; cur_layer < num_activated_layers; ++cur_layer) {
                uint32_t index = num_activated_layers - cur_layer - 1;
                loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "   %s", activated_layers[index].name);
                loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "           Type: %s",
                           activated_layers[index].is_implicit ? "Implicit" : "Explicit");
                if (activated_layers[index].is_implicit) {
                    loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "               Disable Env Var:  %s",
                               activated_layers[index].disable_env);
                }
                loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "           Manifest: %s", activated_layers[index].manifest);
                loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "           Library:  %s", activated_layers[index].library);
                loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "     ||");
            }
            loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "   <Drivers>\n");
        }

        res = fpCreateInstance(&loader_create_info, pAllocator, created_instance);
    } else {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0, "loader_create_instance_chain: Failed to find \'vkCreateInstance\'");
        // Couldn't find CreateInstance function!
        res = VK_ERROR_INITIALIZATION_FAILED;
    }

    if (res == VK_SUCCESS) {
        loader_init_instance_core_dispatch_table(&inst->disp->layer_inst_disp, next_gipa, *created_instance);
        inst->instance = *created_instance;
    }

    return res;
}

void loader_activate_instance_layer_extensions(struct loader_instance *inst, VkInstance created_inst) {
    loader_init_instance_extension_dispatch_table(&inst->disp->layer_inst_disp, inst->disp->layer_inst_disp.GetInstanceProcAddr,
                                                  created_inst);
}

VkResult loader_create_device_chain(const VkPhysicalDevice pd, const VkDeviceCreateInfo *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator, const struct loader_instance *inst,
                                    struct loader_device *dev, PFN_vkGetInstanceProcAddr callingLayer,
                                    PFN_vkGetDeviceProcAddr *layerNextGDPA) {
    uint32_t num_activated_layers = 0;
    struct activated_layer_info *activated_layers = NULL;
    VkLayerDeviceLink *layer_device_link_info;
    VkLayerDeviceCreateInfo chain_info;
    VkDeviceCreateInfo loader_create_info;
    VkResult res;

    PFN_vkGetDeviceProcAddr fpGDPA = NULL, nextGDPA = loader_gpa_device_internal;
    PFN_vkGetInstanceProcAddr fpGIPA = NULL, nextGIPA = loader_gpa_instance_internal;

    memcpy(&loader_create_info, pCreateInfo, sizeof(VkDeviceCreateInfo));

    // Before we continue, we need to find out if the KHR_device_group extension is in the enabled list.  If it is, we then
    // need to look for the corresponding VkDeviceGroupDeviceCreateInfoKHR struct in the device list.  This is because we
    // need to replace all the incoming physical device values (which are really loader trampoline physical device values)
    // with the layer/ICD version.
    {
        VkBaseOutStructure *pNext = (VkBaseOutStructure *)loader_create_info.pNext;
        VkBaseOutStructure *pPrev = (VkBaseOutStructure *)&loader_create_info;
        while (NULL != pNext) {
            if (VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO == pNext->sType) {
                VkDeviceGroupDeviceCreateInfoKHR *cur_struct = (VkDeviceGroupDeviceCreateInfoKHR *)pNext;
                if (0 < cur_struct->physicalDeviceCount && NULL != cur_struct->pPhysicalDevices) {
                    VkDeviceGroupDeviceCreateInfoKHR *temp_struct = loader_stack_alloc(sizeof(VkDeviceGroupDeviceCreateInfoKHR));
                    VkPhysicalDevice *phys_dev_array = NULL;
                    if (NULL == temp_struct) {
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    }
                    memcpy(temp_struct, cur_struct, sizeof(VkDeviceGroupDeviceCreateInfoKHR));
                    phys_dev_array = loader_stack_alloc(sizeof(VkPhysicalDevice) * cur_struct->physicalDeviceCount);
                    if (NULL == phys_dev_array) {
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    }

                    // Before calling down, replace the incoming physical device values (which are really loader trampoline
                    // physical devices) with the next layer (or possibly even the terminator) physical device values.
                    struct loader_physical_device_tramp *cur_tramp;
                    for (uint32_t phys_dev = 0; phys_dev < cur_struct->physicalDeviceCount; phys_dev++) {
                        cur_tramp = (struct loader_physical_device_tramp *)cur_struct->pPhysicalDevices[phys_dev];
                        phys_dev_array[phys_dev] = cur_tramp->phys_dev;
                    }
                    temp_struct->pPhysicalDevices = phys_dev_array;

                    // Replace the old struct in the pNext chain with this one.
                    pPrev->pNext = (VkBaseOutStructure *)temp_struct;
                }
                break;
            }

            pPrev = pNext;
            pNext = pNext->pNext;
        }
    }
    if (dev->expanded_activated_layer_list.count > 0) {
        layer_device_link_info = loader_stack_alloc(sizeof(VkLayerDeviceLink) * dev->expanded_activated_layer_list.count);
        if (!layer_device_link_info) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_create_device_chain: Failed to alloc Device objects for layer. Skipping Layer.");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        activated_layers = loader_stack_alloc(sizeof(struct activated_layer_info) * inst->expanded_activated_layer_list.count);
        if (!activated_layers) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_create_device_chain: Failed to alloc activated layer storage array");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        chain_info.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
        chain_info.function = VK_LAYER_LINK_INFO;
        chain_info.u.pLayerInfo = NULL;
        chain_info.pNext = loader_create_info.pNext;
        loader_create_info.pNext = &chain_info;

        bool done = false;

        // Create instance chain of enabled layers
        for (int32_t i = dev->expanded_activated_layer_list.count - 1; i >= 0; i--) {
            struct loader_layer_properties *layer_prop = &dev->expanded_activated_layer_list.list[i];
            loader_platform_dl_handle lib_handle;

            // Skip it if a Layer with the same name has been already successfully activated
            if (loader_names_array_has_layer_property(&layer_prop->info, num_activated_layers, activated_layers)) {
                continue;
            }

            lib_handle = loader_open_layer_file(inst, "device", layer_prop);
            if (!lib_handle || done) {
                continue;
            }

            // The Get*ProcAddr pointers will already be filled in if they were received from either the json file or the
            // version negotiation
            if ((fpGIPA = layer_prop->functions.get_instance_proc_addr) == NULL) {
                if (strlen(layer_prop->functions.str_gipa) == 0) {
                    fpGIPA = (PFN_vkGetInstanceProcAddr)loader_platform_get_proc_address(lib_handle, "vkGetInstanceProcAddr");
                    layer_prop->functions.get_instance_proc_addr = fpGIPA;
                } else
                    fpGIPA =
                        (PFN_vkGetInstanceProcAddr)loader_platform_get_proc_address(lib_handle, layer_prop->functions.str_gipa);
                if (!fpGIPA) {
                    loader_log(inst, VULKAN_LOADER_ERROR_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                               "loader_create_device_chain: Failed to find \'vkGetInstanceProcAddr\' in layer %s.  Skipping layer.",
                               layer_prop->lib_name);
                    continue;
                }
            }

            if (fpGIPA == callingLayer) {
                if (layerNextGDPA != NULL) {
                    *layerNextGDPA = nextGDPA;
                }
                done = true;
                continue;
            }

            if ((fpGDPA = layer_prop->functions.get_device_proc_addr) == NULL) {
                if (strlen(layer_prop->functions.str_gdpa) == 0) {
                    fpGDPA = (PFN_vkGetDeviceProcAddr)loader_platform_get_proc_address(lib_handle, "vkGetDeviceProcAddr");
                    layer_prop->functions.get_device_proc_addr = fpGDPA;
                } else
                    fpGDPA = (PFN_vkGetDeviceProcAddr)loader_platform_get_proc_address(lib_handle, layer_prop->functions.str_gdpa);
                if (!fpGDPA) {
                    loader_log(inst, VULKAN_LOADER_INFO_BIT | VULKAN_LOADER_LAYER_BIT, 0,
                               "Failed to find vkGetDeviceProcAddr in layer %s", layer_prop->lib_name);
                    continue;
                }
            }

            layer_device_link_info[num_activated_layers].pNext = chain_info.u.pLayerInfo;
            layer_device_link_info[num_activated_layers].pfnNextGetInstanceProcAddr = nextGIPA;
            layer_device_link_info[num_activated_layers].pfnNextGetDeviceProcAddr = nextGDPA;
            chain_info.u.pLayerInfo = &layer_device_link_info[num_activated_layers];
            nextGIPA = fpGIPA;
            nextGDPA = fpGDPA;

            activated_layers[num_activated_layers].name = layer_prop->info.layerName;
            activated_layers[num_activated_layers].manifest = layer_prop->manifest_file_name;
            activated_layers[num_activated_layers].library = layer_prop->lib_name;
            activated_layers[num_activated_layers].is_implicit = !(layer_prop->type_flags & VK_LAYER_TYPE_FLAG_EXPLICIT_LAYER);
            if (activated_layers[num_activated_layers].is_implicit) {
                activated_layers[num_activated_layers].disable_env = layer_prop->disable_env_var.name;
            }

            loader_log(inst, VULKAN_LOADER_INFO_BIT | VULKAN_LOADER_LAYER_BIT, 0, "Inserted device layer %s (%s)",
                       layer_prop->info.layerName, layer_prop->lib_name);

            num_activated_layers++;
        }
    }

    VkDevice created_device = (VkDevice)dev;
    PFN_vkCreateDevice fpCreateDevice = (PFN_vkCreateDevice)nextGIPA(inst->instance, "vkCreateDevice");
    if (fpCreateDevice) {
        VkLayerDeviceCreateInfo create_info_disp;

        create_info_disp.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
        create_info_disp.function = VK_LOADER_DATA_CALLBACK;

        create_info_disp.u.pfnSetDeviceLoaderData = vkSetDeviceDispatch;

        // If layer debugging is enabled, let's print out the full callstack with layers in their
        // defined order.
        if ((loader_get_debug_level() & VULKAN_LOADER_LAYER_BIT) != 0) {
            loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "vkCreateDevice layer callstack setup to:");
            loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "   <Application>");
            loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "     ||");
            loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "   <Loader>");
            loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "     ||");
            for (uint32_t cur_layer = 0; cur_layer < num_activated_layers; ++cur_layer) {
                uint32_t index = num_activated_layers - cur_layer - 1;
                loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "   %s", activated_layers[index].name);
                loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "           Type: %s",
                           activated_layers[index].is_implicit ? "Implicit" : "Explicit");
                if (activated_layers[index].is_implicit) {
                    loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "               Disable Env Var:  %s",
                               activated_layers[index].disable_env);
                }
                loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "           Manifest: %s", activated_layers[index].manifest);
                loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "           Library:  %s", activated_layers[index].library);
                loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "     ||");
            }
            loader_log(inst, VULKAN_LOADER_LAYER_BIT, 0, "   <Device>\n");
        }

        create_info_disp.pNext = loader_create_info.pNext;
        loader_create_info.pNext = &create_info_disp;
        res = fpCreateDevice(pd, &loader_create_info, pAllocator, &created_device);
        if (res != VK_SUCCESS) {
            return res;
        }
        dev->chain_device = created_device;
    } else {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "loader_create_device_chain: Failed to find \'vkCreateDevice\' in layers or ICD");
        // Couldn't find CreateDevice function!
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Initialize device dispatch table
    loader_init_device_dispatch_table(&dev->loader_dispatch, nextGDPA, dev->chain_device);

    return res;
}

VkResult loader_validate_layers(const struct loader_instance *inst, const uint32_t layer_count,
                                const char *const *ppEnabledLayerNames, const struct loader_layer_list *list) {
    struct loader_layer_properties *prop;

    if (layer_count > 0 && ppEnabledLayerNames == NULL) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "loader_validate_instance_layers: ppEnabledLayerNames is NULL but enabledLayerCount is greater than zero");
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    for (uint32_t i = 0; i < layer_count; i++) {
        VkStringErrorFlags result = vk_string_validate(MaxLoaderStringLength, ppEnabledLayerNames[i]);
        if (result != VK_STRING_ERROR_NONE) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_validate_layers: ppEnabledLayerNames contains string that is too long or is badly formed");
            return VK_ERROR_LAYER_NOT_PRESENT;
        }

        prop = loader_find_layer_property(ppEnabledLayerNames[i], list);
        if (NULL == prop) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_validate_layers: Layer %d does not exist in the list of available layers", i);
            return VK_ERROR_LAYER_NOT_PRESENT;
        }
    }
    return VK_SUCCESS;
}

VkResult loader_validate_instance_extensions(struct loader_instance *inst, const struct loader_extension_list *icd_exts,
                                             const struct loader_layer_list *instance_layers,
                                             const VkInstanceCreateInfo *pCreateInfo) {
    VkExtensionProperties *extension_prop;
    char *env_value;
    bool check_if_known = true;
    VkResult res = VK_SUCCESS;

    struct loader_layer_list active_layers;
    struct loader_layer_list expanded_layers;
    memset(&active_layers, 0, sizeof(active_layers));
    memset(&expanded_layers, 0, sizeof(expanded_layers));

    if (pCreateInfo->enabledExtensionCount > 0 && pCreateInfo->ppEnabledExtensionNames == NULL) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "loader_validate_instance_extensions: Instance ppEnabledExtensionNames is NULL but enabledExtensionCount is "
                   "greater than zero");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    if (!loader_init_layer_list(inst, &active_layers)) {
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }
    if (!loader_init_layer_list(inst, &expanded_layers)) {
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }

    // Build the lists of active layers (including metalayers) and expanded layers (with metalayers resolved to their
    // components)
    loader_add_implicit_layers(inst, &active_layers, &expanded_layers, instance_layers);
    res = loader_add_environment_layers(inst, VK_LAYER_TYPE_FLAG_EXPLICIT_LAYER, ENABLED_LAYERS_ENV, &active_layers,
                                        &expanded_layers, instance_layers);
    if (res != VK_SUCCESS) {
        goto out;
    }
    res = loader_add_layer_names_to_list(inst, &active_layers, &expanded_layers, pCreateInfo->enabledLayerCount,
                                         pCreateInfo->ppEnabledLayerNames, instance_layers);
    if (VK_SUCCESS != res) {
        goto out;
    }

    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        VkStringErrorFlags result = vk_string_validate(MaxLoaderStringLength, pCreateInfo->ppEnabledExtensionNames[i]);
        if (result != VK_STRING_ERROR_NONE) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_validate_instance_extensions: Instance ppEnabledExtensionNames contains "
                       "string that is too long or is badly formed");
            res = VK_ERROR_EXTENSION_NOT_PRESENT;
            goto out;
        }

        // Check if a user wants to disable the instance extension filtering behavior
        env_value = loader_getenv("VK_LOADER_DISABLE_INST_EXT_FILTER", inst);
        if (NULL != env_value && atoi(env_value) != 0) {
            check_if_known = false;
        }
        loader_free_getenv(env_value, inst);

        if (check_if_known) {
            // See if the extension is in the list of supported extensions
            bool found = false;
            for (uint32_t j = 0; LOADER_INSTANCE_EXTENSIONS[j] != NULL; j++) {
                if (strcmp(pCreateInfo->ppEnabledExtensionNames[i], LOADER_INSTANCE_EXTENSIONS[j]) == 0) {
                    found = true;
                    break;
                }
            }

            // If it isn't in the list, return an error
            if (!found) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "loader_validate_instance_extensions: Extension %s not found in list of known instance extensions.",
                           pCreateInfo->ppEnabledExtensionNames[i]);
                res = VK_ERROR_EXTENSION_NOT_PRESENT;
                goto out;
            }
        }

        extension_prop = get_extension_property(pCreateInfo->ppEnabledExtensionNames[i], icd_exts);

        if (extension_prop) {
            continue;
        }

        extension_prop = NULL;

        // Not in global list, search layer extension lists
        struct loader_layer_properties *layer_prop = NULL;
        for (uint32_t j = 0; NULL == extension_prop && j < expanded_layers.count; ++j) {
            extension_prop =
                get_extension_property(pCreateInfo->ppEnabledExtensionNames[i], &expanded_layers.list[j].instance_extension_list);
            if (extension_prop) {
                // Found the extension in one of the layers enabled by the app.
                break;
            }

            layer_prop = loader_find_layer_property(expanded_layers.list[j].info.layerName, instance_layers);
            if (NULL == layer_prop) {
                // Should NOT get here, loader_validate_layers should have already filtered this case out.
                continue;
            }
        }

        if (!extension_prop) {
            // Didn't find extension name in any of the global layers, error out
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_validate_instance_extensions: Instance extension %s not supported by available ICDs or enabled "
                       "layers.",
                       pCreateInfo->ppEnabledExtensionNames[i]);
            res = VK_ERROR_EXTENSION_NOT_PRESENT;
            goto out;
        }
    }

out:
    loader_destroy_layer_list(inst, NULL, &active_layers);
    loader_destroy_layer_list(inst, NULL, &expanded_layers);
    return res;
}

VkResult loader_validate_device_extensions(struct loader_instance *this_instance,
                                           const struct loader_layer_list *activated_device_layers,
                                           const struct loader_extension_list *icd_exts, const VkDeviceCreateInfo *pCreateInfo) {
    VkExtensionProperties *extension_prop;
    struct loader_layer_properties *layer_prop;

    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        VkStringErrorFlags result = vk_string_validate(MaxLoaderStringLength, pCreateInfo->ppEnabledExtensionNames[i]);
        if (result != VK_STRING_ERROR_NONE) {
            loader_log(this_instance, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_validate_device_extensions: Device ppEnabledExtensionNames contains "
                       "string that is too long or is badly formed");
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        const char *extension_name = pCreateInfo->ppEnabledExtensionNames[i];
        extension_prop = get_extension_property(extension_name, icd_exts);

        if (extension_prop) {
            continue;
        }

        // Not in global list, search activated layer extension lists
        for (uint32_t j = 0; j < activated_device_layers->count; j++) {
            layer_prop = &activated_device_layers->list[j];

            extension_prop = get_dev_extension_property(extension_name, &layer_prop->device_extension_list);
            if (extension_prop) {
                // Found the extension in one of the layers enabled by the app.
                break;
            }
        }

        if (!extension_prop) {
            // Didn't find extension name in any of the device layers, error out
            loader_log(this_instance, VULKAN_LOADER_ERROR_BIT, 0,
                       "loader_validate_device_extensions: Device extension %s not supported by selected physical device "
                       "or enabled layers.",
                       pCreateInfo->ppEnabledExtensionNames[i]);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    return VK_SUCCESS;
}

// Terminator functions for the Instance chain
// All named terminator_<Vulkan API name>
VKAPI_ATTR VkResult VKAPI_CALL terminator_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                         const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
    struct loader_icd_term *icd_term;
    VkExtensionProperties *prop;
    char **filtered_extension_names = NULL;
    VkInstanceCreateInfo icd_create_info;
    VkResult res = VK_SUCCESS;
    bool one_icd_successful = false;

    struct loader_instance *ptr_instance = (struct loader_instance *)*pInstance;
    if (NULL == ptr_instance) {
        loader_log(ptr_instance, VULKAN_LOADER_WARN_BIT, 0,
                   "terminator_CreateInstance: Loader instance pointer null encountered.  Possibly set by active layer. (Policy "
                   "#LLP_LAYER_21)");
    } else if (LOADER_MAGIC_NUMBER != ptr_instance->magic) {
        loader_log(ptr_instance, VULKAN_LOADER_WARN_BIT, 0,
                   "terminator_CreateInstance: Instance pointer (%p) has invalid MAGIC value 0x%08x. Instance value possibly "
                   "corrupted by active layer (Policy #LLP_LAYER_21).  ",
                   ptr_instance->magic);
    }

    memcpy(&icd_create_info, pCreateInfo, sizeof(icd_create_info));

    icd_create_info.enabledLayerCount = 0;
    icd_create_info.ppEnabledLayerNames = NULL;

    // NOTE: Need to filter the extensions to only those supported by the ICD.
    //       No ICD will advertise support for layers. An ICD library could
    //       support a layer, but it would be independent of the actual ICD,
    //       just in the same library.
    uint32_t extension_count = pCreateInfo->enabledExtensionCount;
#ifdef LOADER_ENABLE_LINUX_SORT
    extension_count += 1;
#endif  // LOADER_ENABLE_LINUX_SORT
    filtered_extension_names = loader_stack_alloc(extension_count * sizeof(char *));
    if (!filtered_extension_names) {
        loader_log(ptr_instance, VULKAN_LOADER_ERROR_BIT, 0,
                   "terminator_CreateInstance: Failed create extension name array for %d extensions", extension_count);
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }
    icd_create_info.ppEnabledExtensionNames = (const char *const *)filtered_extension_names;

    // Determine if Get Physical Device Properties 2 is available to this Instance
    if (pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->apiVersion >= VK_API_VERSION_1_1) {
        ptr_instance->supports_get_dev_prop_2 = true;
    } else {
        for (uint32_t j = 0; j < pCreateInfo->enabledExtensionCount; j++) {
            if (!strcmp(pCreateInfo->ppEnabledExtensionNames[j], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
                ptr_instance->supports_get_dev_prop_2 = true;
                break;
            }
        }
    }

    for (uint32_t i = 0; i < ptr_instance->icd_tramp_list.count; i++) {
        icd_term = loader_icd_add(ptr_instance, &ptr_instance->icd_tramp_list.scanned_list[i]);
        if (NULL == icd_term) {
            loader_log(ptr_instance, VULKAN_LOADER_ERROR_BIT, 0,
                       "terminator_CreateInstance: Failed to add ICD %d to ICD trampoline list.", i);
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto out;
        }

        // If any error happens after here, we need to remove the ICD from the list,
        // because we've already added it, but haven't validated it

        // Make sure that we reset the pApplicationInfo so we don't get an old pointer
        icd_create_info.pApplicationInfo = pCreateInfo->pApplicationInfo;
        icd_create_info.enabledExtensionCount = 0;
        struct loader_extension_list icd_exts;

        loader_log(ptr_instance, VULKAN_LOADER_DEBUG_BIT, 0, "Build ICD instance extension list");
        // traverse scanned icd list adding non-duplicate extensions to the list
        res = loader_init_generic_list(ptr_instance, (struct loader_generic_list *)&icd_exts, sizeof(VkExtensionProperties));
        if (VK_ERROR_OUT_OF_HOST_MEMORY == res) {
            // If out of memory, bail immediately.
            goto out;
        } else if (VK_SUCCESS != res) {
            // Something bad happened with this ICD, so free it and try the
            // next.
            ptr_instance->icd_terms = icd_term->next;
            icd_term->next = NULL;
            loader_icd_destroy(ptr_instance, icd_term, pAllocator);
            continue;
        }

        res = loader_add_instance_extensions(ptr_instance, icd_term->scanned_icd->EnumerateInstanceExtensionProperties,
                                             icd_term->scanned_icd->lib_name, &icd_exts);
        if (VK_SUCCESS != res) {
            loader_destroy_generic_list(ptr_instance, (struct loader_generic_list *)&icd_exts);
            if (VK_ERROR_OUT_OF_HOST_MEMORY == res) {
                // If out of memory, bail immediately.
                goto out;
            } else {
                // Something bad happened with this ICD, so free it and try the next.
                ptr_instance->icd_terms = icd_term->next;
                icd_term->next = NULL;
                loader_icd_destroy(ptr_instance, icd_term, pAllocator);
                continue;
            }
        }

        for (uint32_t j = 0; j < pCreateInfo->enabledExtensionCount; j++) {
            prop = get_extension_property(pCreateInfo->ppEnabledExtensionNames[j], &icd_exts);
            if (prop) {
                filtered_extension_names[icd_create_info.enabledExtensionCount] = (char *)pCreateInfo->ppEnabledExtensionNames[j];
                icd_create_info.enabledExtensionCount++;
            }
        }
#ifdef LOADER_ENABLE_LINUX_SORT
        // Force on "VK_KHR_get_physical_device_properties2" for Linux as we use it for GPU sorting.
        if (icd_term->scanned_icd->api_version < VK_API_VERSION_1_1) {
            prop = get_extension_property(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, &icd_exts);
            if (prop) {
                filtered_extension_names[icd_create_info.enabledExtensionCount] =
                    (char *)VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
                icd_create_info.enabledExtensionCount++;
            }
        }
#endif  // LOADER_ENABLE_LINUX_SORT

        // Determine if vkGetPhysicalDeviceProperties2 is available to this Instance
        if (icd_term->scanned_icd->api_version >= VK_API_VERSION_1_1) {
            icd_term->supports_get_dev_prop_2 = true;
        } else {
            for (uint32_t j = 0; j < icd_create_info.enabledExtensionCount; j++) {
                if (!strcmp(filtered_extension_names[j], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
                    icd_term->supports_get_dev_prop_2 = true;
                    break;
                }
            }
        }

        loader_destroy_generic_list(ptr_instance, (struct loader_generic_list *)&icd_exts);

        // Get the driver version from vkEnumerateInstanceVersion
        uint32_t icd_version = VK_API_VERSION_1_0;
        VkResult icd_result = VK_SUCCESS;
        if (icd_term->scanned_icd->api_version >= VK_API_VERSION_1_1) {
            PFN_vkEnumerateInstanceVersion icd_enumerate_instance_version =
                (PFN_vkEnumerateInstanceVersion)icd_term->scanned_icd->GetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
            if (icd_enumerate_instance_version != NULL) {
                icd_result = icd_enumerate_instance_version(&icd_version);
                if (icd_result != VK_SUCCESS) {
                    icd_version = VK_API_VERSION_1_0;
                    loader_log(ptr_instance, VULKAN_LOADER_DEBUG_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                               "terminator_CreateInstance: ICD \"%s\" vkEnumerateInstanceVersion returned error. The ICD will be "
                               "treated as a 1.0 ICD",
                               icd_term->scanned_icd->lib_name);
                }
            }
        }

        // Create an instance, substituting the version to 1.0 if necessary
        VkApplicationInfo icd_app_info;
        uint32_t icd_version_nopatch = VK_MAKE_VERSION(VK_VERSION_MAJOR(icd_version), VK_VERSION_MINOR(icd_version), 0);
        uint32_t requested_version = pCreateInfo == NULL || pCreateInfo->pApplicationInfo == NULL
                                         ? VK_API_VERSION_1_0
                                         : pCreateInfo->pApplicationInfo->apiVersion;
        if ((requested_version != 0) && (icd_version_nopatch == VK_API_VERSION_1_0)) {
            if (icd_create_info.pApplicationInfo == NULL) {
                memset(&icd_app_info, 0, sizeof(icd_app_info));
            } else {
                memcpy(&icd_app_info, icd_create_info.pApplicationInfo, sizeof(icd_app_info));
            }
            icd_app_info.apiVersion = icd_version;
            icd_create_info.pApplicationInfo = &icd_app_info;
        }
        icd_result =
            ptr_instance->icd_tramp_list.scanned_list[i].CreateInstance(&icd_create_info, pAllocator, &(icd_term->instance));
        if (VK_ERROR_OUT_OF_HOST_MEMORY == icd_result) {
            // If out of memory, bail immediately.
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto out;
        } else if (VK_SUCCESS != icd_result) {
            loader_log(ptr_instance, VULKAN_LOADER_WARN_BIT, 0,
                       "terminator_CreateInstance: Failed to CreateInstance in ICD %d.  Skipping ICD.", i);
            ptr_instance->icd_terms = icd_term->next;
            icd_term->next = NULL;
            loader_icd_destroy(ptr_instance, icd_term, pAllocator);
            continue;
        }

        if (!loader_icd_init_entries(icd_term, icd_term->instance,
                                     ptr_instance->icd_tramp_list.scanned_list[i].GetInstanceProcAddr)) {
            loader_log(ptr_instance, VULKAN_LOADER_WARN_BIT, 0,
                       "terminator_CreateInstance: Failed to CreateInstance and find entrypoints with ICD.  Skipping ICD.");
            ptr_instance->icd_terms = icd_term->next;
            icd_term->next = NULL;
            loader_icd_destroy(ptr_instance, icd_term, pAllocator);
            continue;
        }

        if (ptr_instance->icd_tramp_list.scanned_list[i].interface_version < 3 &&
            (
#ifdef VK_USE_PLATFORM_XLIB_KHR
                NULL != icd_term->dispatch.CreateXlibSurfaceKHR ||
#endif  // VK_USE_PLATFORM_XLIB_KHR
#ifdef VK_USE_PLATFORM_XCB_KHR
                NULL != icd_term->dispatch.CreateXcbSurfaceKHR ||
#endif  // VK_USE_PLATFORM_XCB_KHR
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
                NULL != icd_term->dispatch.CreateWaylandSurfaceKHR ||
#endif  // VK_USE_PLATFORM_WAYLAND_KHR
#ifdef VK_USE_PLATFORM_ANDROID_KHR
                NULL != icd_term->dispatch.CreateAndroidSurfaceKHR ||
#endif  // VK_USE_PLATFORM_ANDROID_KHR
#ifdef VK_USE_PLATFORM_WIN32_KHR
                NULL != icd_term->dispatch.CreateWin32SurfaceKHR ||
#endif  // VK_USE_PLATFORM_WIN32_KHR
                NULL != icd_term->dispatch.DestroySurfaceKHR)) {
            loader_log(ptr_instance, VULKAN_LOADER_WARN_BIT, 0,
                       "terminator_CreateInstance: Driver %s supports interface version %u but still exposes VkSurfacekHR"
                       " create/destroy entrypoints (Policy #LDP_DRIVER_8)",
                       ptr_instance->icd_tramp_list.scanned_list[i].lib_name,
                       ptr_instance->icd_tramp_list.scanned_list[i].interface_version);
        }

        // If we made it this far, at least one ICD was successful
        one_icd_successful = true;
    }

    // For vkGetPhysicalDeviceProperties2, at least one ICD needs to support the extension for the
    // instance to have it
    if (ptr_instance->supports_get_dev_prop_2) {
        bool at_least_one_supports = false;
        icd_term = ptr_instance->icd_terms;
        while (icd_term != NULL) {
            if (icd_term->supports_get_dev_prop_2) {
                at_least_one_supports = true;
                break;
            }
            icd_term = icd_term->next;
        }
        if (!at_least_one_supports) {
            ptr_instance->supports_get_dev_prop_2 = false;
        }
    }

    // If no ICDs were added to instance list and res is unchanged from it's initial value, the loader was unable to
    // find a suitable ICD.
    if (VK_SUCCESS == res && (ptr_instance->icd_terms == NULL || !one_icd_successful)) {
        res = VK_ERROR_INCOMPATIBLE_DRIVER;
    }

out:

    ptr_instance->create_terminator_invalid_extension = false;

    if (VK_SUCCESS != res) {
        if (VK_ERROR_EXTENSION_NOT_PRESENT == res) {
            ptr_instance->create_terminator_invalid_extension = true;
        }

        while (NULL != ptr_instance->icd_terms) {
            icd_term = ptr_instance->icd_terms;
            ptr_instance->icd_terms = icd_term->next;
            if (NULL != icd_term->instance) {
                icd_term->dispatch.DestroyInstance(icd_term->instance, pAllocator);
            }
            loader_icd_destroy(ptr_instance, icd_term, pAllocator);
        }
    } else {
        // Check for enabled extensions here to setup the loader structures so the loader knows what extensions
        // it needs to worry about.
        // We do it here and again above the layers in the trampoline function since the trampoline function
        // may think different extensions are enabled than what's down here.
        // This is why we don't clear inside of these function calls.
        // The clearing should actually be handled by the overall memset of the pInstance structure in the
        // trampoline.
        wsi_create_instance(ptr_instance, pCreateInfo);
        debug_utils_CreateInstance(ptr_instance, pCreateInfo);
        extensions_create_instance(ptr_instance, pCreateInfo);
    }

    return res;
}

VKAPI_ATTR void VKAPI_CALL terminator_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    struct loader_instance *ptr_instance = loader_instance(instance);
    if (NULL == ptr_instance) {
        return;
    }
    struct loader_icd_term *icd_terms = ptr_instance->icd_terms;
    struct loader_icd_term *next_icd_term;

    // Remove this instance from the list of instances:
    struct loader_instance *prev = NULL;
    struct loader_instance *next = loader.instances;
    while (next != NULL) {
        if (next == ptr_instance) {
            // Remove this instance from the list:
            if (prev)
                prev->next = next->next;
            else
                loader.instances = next->next;
            break;
        }
        prev = next;
        next = next->next;
    }

    while (NULL != icd_terms) {
        if (icd_terms->instance) {
            icd_terms->dispatch.DestroyInstance(icd_terms->instance, pAllocator);
        }
        next_icd_term = icd_terms->next;
        icd_terms->instance = VK_NULL_HANDLE;
        loader_icd_destroy(ptr_instance, icd_terms, pAllocator);

        icd_terms = next_icd_term;
    }

    loader_delete_layer_list_and_properties(ptr_instance, &ptr_instance->instance_layer_list);
    loader_scanned_icd_clear(ptr_instance, &ptr_instance->icd_tramp_list);
    loader_destroy_generic_list(ptr_instance, (struct loader_generic_list *)&ptr_instance->ext_list);
    if (NULL != ptr_instance->phys_devs_term) {
        for (uint32_t i = 0; i < ptr_instance->phys_dev_count_term; i++) {
            loader_instance_heap_free(ptr_instance, ptr_instance->phys_devs_term[i]);
        }
        loader_instance_heap_free(ptr_instance, ptr_instance->phys_devs_term);
    }
    if (NULL != ptr_instance->phys_dev_groups_term) {
        for (uint32_t i = 0; i < ptr_instance->phys_dev_group_count_term; i++) {
            loader_instance_heap_free(ptr_instance, ptr_instance->phys_dev_groups_term[i]);
        }
        loader_instance_heap_free(ptr_instance, ptr_instance->phys_dev_groups_term);
    }
    loader_free_dev_ext_table(ptr_instance);
    loader_free_phys_dev_ext_table(ptr_instance);
}

VKAPI_ATTR VkResult VKAPI_CALL terminator_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                                                       const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    VkResult res = VK_SUCCESS;
    struct loader_physical_device_term *phys_dev_term;
    phys_dev_term = (struct loader_physical_device_term *)physicalDevice;
    struct loader_icd_term *icd_term = phys_dev_term->this_icd_term;

    struct loader_device *dev = (struct loader_device *)*pDevice;
    PFN_vkCreateDevice fpCreateDevice = icd_term->dispatch.CreateDevice;
    struct loader_extension_list icd_exts;

    VkBaseOutStructure *caller_dgci_container = NULL;
    VkDeviceGroupDeviceCreateInfoKHR *caller_dgci = NULL;

    dev->phys_dev_term = phys_dev_term;

    icd_exts.list = NULL;

    if (fpCreateDevice == NULL) {
        loader_log(icd_term->this_instance, VULKAN_LOADER_ERROR_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                   "terminator_CreateDevice: No vkCreateDevice command exposed by ICD %s", icd_term->scanned_icd->lib_name);
        res = VK_ERROR_INITIALIZATION_FAILED;
        goto out;
    }

    VkDeviceCreateInfo localCreateInfo;
    memcpy(&localCreateInfo, pCreateInfo, sizeof(localCreateInfo));

    // NOTE: Need to filter the extensions to only those supported by the ICD.
    //       No ICD will advertise support for layers. An ICD library could support a layer,
    //       but it would be independent of the actual ICD, just in the same library.
    char **filtered_extension_names = NULL;
    if (0 < pCreateInfo->enabledExtensionCount) {
        filtered_extension_names = loader_stack_alloc(pCreateInfo->enabledExtensionCount * sizeof(char *));
        if (NULL == filtered_extension_names) {
            loader_log(icd_term->this_instance, VULKAN_LOADER_ERROR_BIT, 0,
                       "terminator_CreateDevice: Failed to create extension name storage for %d extensions",
                       pCreateInfo->enabledExtensionCount);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    localCreateInfo.enabledLayerCount = 0;
    localCreateInfo.ppEnabledLayerNames = NULL;

    localCreateInfo.enabledExtensionCount = 0;
    localCreateInfo.ppEnabledExtensionNames = (const char *const *)filtered_extension_names;

    // Get the physical device (ICD) extensions
    res = loader_init_generic_list(icd_term->this_instance, (struct loader_generic_list *)&icd_exts, sizeof(VkExtensionProperties));
    if (VK_SUCCESS != res) {
        goto out;
    }

    res = loader_add_device_extensions(icd_term->this_instance, icd_term->dispatch.EnumerateDeviceExtensionProperties,
                                       phys_dev_term->phys_dev, icd_term->scanned_icd->lib_name, &icd_exts);
    if (res != VK_SUCCESS) {
        goto out;
    }

    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        const char *extension_name = pCreateInfo->ppEnabledExtensionNames[i];
        VkExtensionProperties *prop = get_extension_property(extension_name, &icd_exts);
        if (prop) {
            filtered_extension_names[localCreateInfo.enabledExtensionCount] = (char *)extension_name;
            localCreateInfo.enabledExtensionCount++;
        } else {
            loader_log(icd_term->this_instance, VULKAN_LOADER_DEBUG_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                       "vkCreateDevice extension %s not available for devices associated with ICD %s", extension_name,
                       icd_term->scanned_icd->lib_name);
        }
    }

    // Before we continue, If KHX_device_group is the list of enabled and viable extensions, then we then need to look for the
    // corresponding VkDeviceGroupDeviceCreateInfo struct in the device list and replace all the physical device values (which
    // are really loader physical device terminator values) with the ICD versions.
    // if (icd_term->this_instance->enabled_known_extensions.khr_device_group_creation == 1) {
    {
        VkBaseOutStructure *pNext = (VkBaseOutStructure *)localCreateInfo.pNext;
        VkBaseOutStructure *pPrev = (VkBaseOutStructure *)&localCreateInfo;
        while (NULL != pNext) {
            if (VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO == pNext->sType) {
                VkDeviceGroupDeviceCreateInfo *cur_struct = (VkDeviceGroupDeviceCreateInfo *)pNext;
                if (0 < cur_struct->physicalDeviceCount && NULL != cur_struct->pPhysicalDevices) {
                    VkDeviceGroupDeviceCreateInfo *temp_struct = loader_stack_alloc(sizeof(VkDeviceGroupDeviceCreateInfo));
                    VkPhysicalDevice *phys_dev_array = NULL;
                    if (NULL == temp_struct) {
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    }
                    memcpy(temp_struct, cur_struct, sizeof(VkDeviceGroupDeviceCreateInfo));
                    phys_dev_array = loader_stack_alloc(sizeof(VkPhysicalDevice) * cur_struct->physicalDeviceCount);
                    if (NULL == phys_dev_array) {
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    }

                    // Before calling down, replace the incoming physical device values (which are really loader terminator
                    // physical devices) with the ICDs physical device values.
                    struct loader_physical_device_term *cur_term;
                    for (uint32_t phys_dev = 0; phys_dev < cur_struct->physicalDeviceCount; phys_dev++) {
                        cur_term = (struct loader_physical_device_term *)cur_struct->pPhysicalDevices[phys_dev];
                        phys_dev_array[phys_dev] = cur_term->phys_dev;
                    }
                    temp_struct->pPhysicalDevices = phys_dev_array;

                    // Keep track of pointers to restore pNext chain before returning
                    caller_dgci_container = pPrev;
                    caller_dgci = cur_struct;

                    // Replace the old struct in the pNext chain with this one.
                    pPrev->pNext = (VkBaseOutStructure *)temp_struct;
                }
                break;
            }

            pPrev = pNext;
            pNext = pNext->pNext;
        }
    }

    // Handle loader emulation for structs that are not supported by the ICD:
    // Presently, the emulation leaves the pNext chain alone. This means that the ICD will receive items in the chain which
    // are not recognized by the ICD. If this causes the ICD to fail, then the items would have to be removed here. The current
    // implementation does not remove them because copying the pNext chain would be impossible if the loader does not recognize
    // the any of the struct types, as the loader would not know the size to allocate and copy.
    // if (icd_term->dispatch.GetPhysicalDeviceFeatures2 == NULL && icd_term->dispatch.GetPhysicalDeviceFeatures2KHR == NULL) {
    {
        const void *pNext = localCreateInfo.pNext;
        while (pNext != NULL) {
            switch (*(VkStructureType *)pNext) {
                case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: {
                    const VkPhysicalDeviceFeatures2KHR *features = pNext;

                    if (icd_term->dispatch.GetPhysicalDeviceFeatures2 == NULL &&
                        icd_term->dispatch.GetPhysicalDeviceFeatures2KHR == NULL) {
                        loader_log(icd_term->this_instance, VULKAN_LOADER_INFO_BIT, 0,
                                   "vkCreateDevice: Emulating handling of VkPhysicalDeviceFeatures2 in pNext chain for ICD \"%s\"",
                                   icd_term->scanned_icd->lib_name);

                        // Verify that VK_KHR_get_physical_device_properties2 is enabled
                        if (icd_term->this_instance->enabled_known_extensions.khr_get_physical_device_properties2) {
                            localCreateInfo.pEnabledFeatures = &features->features;
                        }
                    }

                    // Leave this item in the pNext chain for now

                    pNext = features->pNext;
                    break;
                }

                case VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO: {
                    const VkDeviceGroupDeviceCreateInfoKHR *group_info = pNext;

                    if (icd_term->dispatch.EnumeratePhysicalDeviceGroups == NULL &&
                        icd_term->dispatch.EnumeratePhysicalDeviceGroupsKHR == NULL) {
                        loader_log(icd_term->this_instance, VULKAN_LOADER_INFO_BIT, 0,
                                   "vkCreateDevice: Emulating handling of VkPhysicalDeviceGroupProperties in pNext chain for "
                                   "ICD \"%s\"",
                                   icd_term->scanned_icd->lib_name);

                        // The group must contain only this one device, since physical device groups aren't actually supported
                        if (group_info->physicalDeviceCount != 1) {
                            loader_log(icd_term->this_instance, VULKAN_LOADER_ERROR_BIT, 0,
                                       "vkCreateDevice: Emulation failed to create device from device group info");
                            res = VK_ERROR_INITIALIZATION_FAILED;
                            goto out;
                        }
                    }

                    // Nothing needs to be done here because we're leaving the item in the pNext chain and because the spec
                    // states that the physicalDevice argument must be included in the device group, and we've already checked
                    // that it is

                    pNext = group_info->pNext;
                    break;
                }

                // Multiview properties are also allowed, but since VK_KHX_multiview is a device extension, we'll just let the
                // ICD handle that error when the user enables the extension here
                default: {
                    const VkBaseInStructure *header = pNext;
                    pNext = header->pNext;
                    break;
                }
            }
        }
    }

    // Every extension that has a loader-defined terminator needs to be marked as enabled or disabled so that we know whether or
    // not to return that terminator when vkGetDeviceProcAddr is called
    for (uint32_t i = 0; i < localCreateInfo.enabledExtensionCount; ++i) {
        if (!strcmp(localCreateInfo.ppEnabledExtensionNames[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            dev->extensions.khr_swapchain_enabled = true;
        } else if (!strcmp(localCreateInfo.ppEnabledExtensionNames[i], VK_KHR_DISPLAY_SWAPCHAIN_EXTENSION_NAME)) {
            dev->extensions.khr_display_swapchain_enabled = true;
        } else if (!strcmp(localCreateInfo.ppEnabledExtensionNames[i], VK_KHR_DEVICE_GROUP_EXTENSION_NAME)) {
            dev->extensions.khr_device_group_enabled = true;
        } else if (!strcmp(localCreateInfo.ppEnabledExtensionNames[i], VK_EXT_DEBUG_MARKER_EXTENSION_NAME)) {
            dev->extensions.ext_debug_marker_enabled = true;
        } else if (!strcmp(localCreateInfo.ppEnabledExtensionNames[i], "VK_EXT_full_screen_exclusive")) {
            dev->extensions.ext_full_screen_exclusive_enabled = true;
        }
    }
    dev->extensions.ext_debug_utils_enabled = icd_term->this_instance->enabled_known_extensions.ext_debug_utils;

    if (!dev->extensions.khr_device_group_enabled) {
        VkPhysicalDeviceProperties properties;
        icd_term->dispatch.GetPhysicalDeviceProperties(phys_dev_term->phys_dev, &properties);
        if (properties.apiVersion >= VK_API_VERSION_1_1) {
            dev->extensions.khr_device_group_enabled = true;
        }
    }

    res = fpCreateDevice(phys_dev_term->phys_dev, &localCreateInfo, pAllocator, &dev->icd_device);
    if (res != VK_SUCCESS) {
        loader_log(icd_term->this_instance, VULKAN_LOADER_ERROR_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                   "terminator_CreateDevice: Failed in ICD %s vkCreateDevice call", icd_term->scanned_icd->lib_name);
        goto out;
    }

    *pDevice = dev->icd_device;
    loader_add_logical_device(icd_term->this_instance, icd_term, dev);

    // Init dispatch pointer in new device object
    loader_init_dispatch(*pDevice, &dev->loader_dispatch);

out:
    if (NULL != icd_exts.list) {
        loader_destroy_generic_list(icd_term->this_instance, (struct loader_generic_list *)&icd_exts);
    }

    // Restore pNext pointer to old VkDeviceGroupDeviceCreateInfoKHX
    // in the chain to maintain consistency for the caller.
    if (caller_dgci_container != NULL) {
        caller_dgci_container->pNext = (VkBaseOutStructure *)caller_dgci;
    }

    return res;
}

VkResult setup_loader_tramp_phys_devs(struct loader_instance *inst) {
    VkResult res = VK_SUCCESS;
    VkPhysicalDevice *local_phys_devs = NULL;
    uint32_t total_count = 0;
    struct loader_physical_device_tramp **new_phys_devs = NULL;

    // Query how many GPUs there
    res = inst->disp->layer_inst_disp.EnumeratePhysicalDevices(inst->instance, &total_count, NULL);
    if (res != VK_SUCCESS) {
        loader_log(
            inst, VULKAN_LOADER_ERROR_BIT, 0,
            "setup_loader_tramp_phys_devs:  Failed during dispatch call of \'vkEnumeratePhysicalDevices\' to lower layers or "
            "loader to get count.");
        goto out;
    }

    // Really use what the total GPU count is since Optimus and other layers may mess
    // the count up.
    total_count = inst->total_gpu_count;

    // Create an array for the new physical devices, which will be stored
    // in the instance for the trampoline code.
    new_phys_devs = (struct loader_physical_device_tramp **)loader_instance_heap_alloc(
        inst, total_count * sizeof(struct loader_physical_device_tramp *), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    if (NULL == new_phys_devs) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "setup_loader_tramp_phys_devs:  Failed to allocate new physical device array of size %d", total_count);
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }
    memset(new_phys_devs, 0, total_count * sizeof(struct loader_physical_device_tramp *));

    // Create a temporary array (on the stack) to keep track of the
    // returned VkPhysicalDevice values.
    local_phys_devs = loader_stack_alloc(sizeof(VkPhysicalDevice) * total_count);
    if (NULL == local_phys_devs) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "setup_loader_tramp_phys_devs:  Failed to allocate local physical device array of size %d", total_count);
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }
    memset(local_phys_devs, 0, sizeof(VkPhysicalDevice) * total_count);

    res = inst->disp->layer_inst_disp.EnumeratePhysicalDevices(inst->instance, &total_count, local_phys_devs);
    if (VK_SUCCESS != res) {
        loader_log(
            inst, VULKAN_LOADER_ERROR_BIT, 0,
            "setup_loader_tramp_phys_devs:  Failed during dispatch call of \'vkEnumeratePhysicalDevices\' to lower layers or "
            "loader to get content.");
        goto out;
    }

    // Copy or create everything to fill the new array of physical devices
    for (uint32_t new_idx = 0; new_idx < total_count; new_idx++) {
        // Check if this physical device is already in the old buffer
        for (uint32_t old_idx = 0; old_idx < inst->phys_dev_count_tramp; old_idx++) {
            if (local_phys_devs[new_idx] == inst->phys_devs_tramp[old_idx]->phys_dev) {
                new_phys_devs[new_idx] = inst->phys_devs_tramp[old_idx];
                break;
            }
        }

        // If this physical device isn't in the old buffer, create it
        if (NULL == new_phys_devs[new_idx]) {
            new_phys_devs[new_idx] = (struct loader_physical_device_tramp *)loader_instance_heap_alloc(
                inst, sizeof(struct loader_physical_device_tramp), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
            if (NULL == new_phys_devs[new_idx]) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "setup_loader_tramp_phys_devs:  Failed to allocate physical device trampoline object %d", new_idx);
                total_count = new_idx;
                res = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto out;
            }

            // Initialize the new physicalDevice object
            loader_set_dispatch((void *)new_phys_devs[new_idx], inst->disp);
            new_phys_devs[new_idx]->this_instance = inst;
            new_phys_devs[new_idx]->phys_dev = local_phys_devs[new_idx];
            new_phys_devs[new_idx]->magic = PHYS_TRAMP_MAGIC_NUMBER;
        }
    }

out:

    if (VK_SUCCESS != res) {
        if (NULL != new_phys_devs) {
            for (uint32_t i = 0; i < total_count; i++) {
                // If an OOM occurred inside the copying of the new physical devices into the existing array
                // will leave some of the old physical devices in the array which may have been copied into
                // the new array, leading to them being freed twice. To avoid this we just make sure to not
                // delete physical devices which were copied.
                bool found = false;
                for (uint32_t old_idx = 0; old_idx < inst->phys_dev_count_tramp; old_idx++) {
                    if (new_phys_devs[i] == inst->phys_devs_tramp[old_idx]) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    loader_instance_heap_free(inst, new_phys_devs[i]);
                }
            }
            loader_instance_heap_free(inst, new_phys_devs);
        }
        total_count = 0;
    } else {
        // Free everything that didn't carry over to the new array of
        // physical devices
        if (NULL != inst->phys_devs_tramp) {
            for (uint32_t i = 0; i < inst->phys_dev_count_tramp; i++) {
                bool found = false;
                for (uint32_t j = 0; j < total_count; j++) {
                    if (inst->phys_devs_tramp[i] == new_phys_devs[j]) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    loader_instance_heap_free(inst, inst->phys_devs_tramp[i]);
                }
            }
            loader_instance_heap_free(inst, inst->phys_devs_tramp);
        }

        // Swap in the new physical device list
        inst->phys_dev_count_tramp = total_count;
        inst->phys_devs_tramp = new_phys_devs;
    }

    return res;
}

#ifdef LOADER_ENABLE_LINUX_SORT
bool is_linux_sort_enabled(struct loader_instance *inst) {
    bool sort_items = inst->supports_get_dev_prop_2;
    char *env_value = loader_getenv("VK_LOADER_DISABLE_SELECT", inst);
    if (NULL != env_value) {
        int32_t int_env_val = atoi(env_value);
        loader_free_getenv(env_value, inst);
        if (int_env_val != 0) {
            sort_items = false;
        }
    }
    return sort_items;
}
#endif  // LOADER_ENABLE_LINUX_SORT

VkResult setup_loader_term_phys_devs(struct loader_instance *inst) {
    VkResult res = VK_SUCCESS;
    struct loader_icd_term *icd_term;
    struct loader_phys_dev_per_icd *icd_phys_dev_array = NULL;
    struct loader_physical_device_term **new_phys_devs = NULL;
    struct LoaderSortedPhysicalDevice *sorted_phys_dev_array = NULL;
    uint32_t icd_idx = 0;
    uint32_t sorted_count = 0;

    inst->total_gpu_count = 0;

    // Allocate something to store the physical device characteristics
    // that we read from each ICD.
    icd_phys_dev_array =
        (struct loader_phys_dev_per_icd *)loader_stack_alloc(sizeof(struct loader_phys_dev_per_icd) * inst->total_icd_count);
    if (NULL == icd_phys_dev_array) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "setup_loader_term_phys_devs:  Failed to allocate temporary ICD Physical device info array of size %d",
                   inst->total_icd_count);
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }
    memset(icd_phys_dev_array, 0, sizeof(struct loader_phys_dev_per_icd) * inst->total_icd_count);

#if defined(_WIN32)
    // Get the physical devices supported by platform sorting mechanism into a separate list
    res = windows_read_sorted_physical_devices(inst, &sorted_phys_dev_array, &sorted_count);
    if (VK_SUCCESS != res) {
        goto out;
    }
#endif

    // For each ICD, query the number of physical devices, and then get an
    // internal value for those physical devices.
    icd_term = inst->icd_terms;
    while (NULL != icd_term) {
        // This is the legacy behavior which should be skipped if EnumerateAdapterPhysicalDevices is available
        // and we successfully enumerated sorted adapters using windows_read_sorted_physical_devices.
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        if (sorted_count && icd_term->scanned_icd->EnumerateAdapterPhysicalDevices != NULL) {
            icd_term = icd_term->next;
            continue;
        }
#endif

        res = icd_term->dispatch.EnumeratePhysicalDevices(icd_term->instance, &icd_phys_dev_array[icd_idx].count, NULL);
        if (VK_SUCCESS != res) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "setup_loader_term_phys_devs:  Call to ICD %d's \'vkEnumeratePhysicalDevices\' failed with error 0x%08x",
                       icd_idx, res);
            goto out;
        }

        icd_phys_dev_array[icd_idx].phys_devs =
            (VkPhysicalDevice *)loader_stack_alloc(icd_phys_dev_array[icd_idx].count * sizeof(VkPhysicalDevice));
        if (NULL == icd_phys_dev_array[icd_idx].phys_devs) {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "setup_loader_term_phys_devs:  Failed to allocate temporary ICD Physical device array for ICD %d of size %d",
                       icd_idx, inst->total_gpu_count);
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto out;
        }

        res = icd_term->dispatch.EnumeratePhysicalDevices(icd_term->instance, &(icd_phys_dev_array[icd_idx].count),
                                                          icd_phys_dev_array[icd_idx].phys_devs);
        if (VK_SUCCESS != res) {
            goto out;
        }
        inst->total_gpu_count += icd_phys_dev_array[icd_idx].count;
        icd_phys_dev_array[icd_idx].this_icd_term = icd_term;
        icd_term = icd_term->next;
        ++icd_idx;
    }

    if (0 == inst->total_gpu_count) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "setup_loader_term_phys_devs:  Failed to detect any valid GPUs in the current config");
        res = VK_ERROR_INITIALIZATION_FAILED;
        goto out;
    }

    new_phys_devs = loader_instance_heap_alloc(inst, sizeof(struct loader_physical_device_term *) * inst->total_gpu_count,
                                               VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    if (NULL == new_phys_devs) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "setup_loader_term_phys_devs:  Failed to allocate new physical device array of size %d", inst->total_gpu_count);
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }
    memset(new_phys_devs, 0, sizeof(struct loader_physical_device_term *) * inst->total_gpu_count);

#ifdef LOADER_ENABLE_LINUX_SORT
    if (is_linux_sort_enabled(inst)) {
        for (uint32_t dev = 0; dev < inst->total_gpu_count; ++dev) {
            new_phys_devs[dev] =
                loader_instance_heap_alloc(inst, sizeof(struct loader_physical_device_term), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
            if (NULL == new_phys_devs[dev]) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "setup_loader_term_phys_devs:  Failed to allocate physical device terminator object %d", dev);
                inst->total_gpu_count = dev;
                res = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto out;
            }
        }

        // Get the physical devices supported by platform sorting mechanism into a separate list
        res = linux_read_sorted_physical_devices(inst, icd_idx, icd_phys_dev_array, new_phys_devs);

        // Keep previously allocated physical device info since apps may already be using that!
        for (uint32_t new_idx = 0; new_idx < inst->total_gpu_count; new_idx++) {
            for (uint32_t old_idx = 0; old_idx < inst->phys_dev_count_term; old_idx++) {
                if (new_phys_devs[new_idx]->phys_dev == inst->phys_devs_term[old_idx]->phys_dev) {
                    loader_log(inst, VULKAN_LOADER_INFO_BIT | VULKAN_LOADER_DRIVER_BIT, 0,
                                "Copying old device %u into new device %u", old_idx, new_idx);
                    // Free the old new_phys_devs info since we're not using it before we assign the new info
                    loader_instance_heap_free(inst, new_phys_devs[new_idx]);
                    new_phys_devs[new_idx] = inst->phys_devs_term[old_idx];
                    break;
                }
            }
        }
        goto out;
    }
#endif  // LOADER_ENABLE_LINUX_SORT

    // Copy or create everything to fill the new array of physical devices
    uint32_t idx = 0;

#if defined(_WIN32)
    // Copy over everything found through sorted enumeration
    for (uint32_t i = 0; i < sorted_count; ++i) {
        for (uint32_t j = 0; j < sorted_phys_dev_array[i].device_count; ++j) {
            // Check if this physical device is already in the old buffer
            if (NULL != inst->phys_devs_term) {
                for (uint32_t old_idx = 0; old_idx < inst->phys_dev_count_term; old_idx++) {
                    if (sorted_phys_dev_array[i].physical_devices[j] == inst->phys_devs_term[old_idx]->phys_dev) {
                        new_phys_devs[idx] = inst->phys_devs_term[old_idx];
                        break;
                    }
                }
            }

            // If this physical device isn't in the old buffer, then we need to create it.
            if (NULL == new_phys_devs[idx]) {
                new_phys_devs[idx] = loader_instance_heap_alloc(inst, sizeof(struct loader_physical_device_term),
                                                                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
                if (NULL == new_phys_devs[idx]) {
                    loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                               "setup_loader_term_phys_devs:  Failed to allocate physical device terminator object %d", idx);
                    inst->total_gpu_count = idx;
                    res = VK_ERROR_OUT_OF_HOST_MEMORY;
                    goto out;
                }

                loader_set_dispatch((void *)new_phys_devs[idx], inst->disp);
                new_phys_devs[idx]->this_icd_term = sorted_phys_dev_array[i].icd_term;
                new_phys_devs[idx]->icd_index = (uint8_t)(sorted_phys_dev_array[i].icd_index);
                new_phys_devs[idx]->phys_dev = sorted_phys_dev_array[i].physical_devices[j];
            }

            // Increment the count of new physical devices
            idx++;
        }
    }
#endif

    // Copy over everything found through EnumeratePhysicalDevices
    for (icd_idx = 0; icd_idx < inst->total_icd_count; icd_idx++) {
        for (uint32_t pd_idx = 0; pd_idx < icd_phys_dev_array[icd_idx].count; pd_idx++) {
            // Check if this physical device is already in the old buffer
            if (NULL != inst->phys_devs_term) {
                for (uint32_t old_idx = 0; old_idx < inst->phys_dev_count_term; old_idx++) {
                    if (icd_phys_dev_array[icd_idx].phys_devs[pd_idx] == inst->phys_devs_term[old_idx]->phys_dev) {
                        new_phys_devs[idx] = inst->phys_devs_term[old_idx];
                        break;
                    }
                }
            }
            // If this physical device isn't in the old buffer, then we
            // need to create it.
            if (NULL == new_phys_devs[idx]) {
                new_phys_devs[idx] = loader_instance_heap_alloc(inst, sizeof(struct loader_physical_device_term),
                                                                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
                if (NULL == new_phys_devs[idx]) {
                    loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                               "setup_loader_term_phys_devs:  Failed to allocate physical device terminator object %d", idx);
                    inst->total_gpu_count = idx;
                    res = VK_ERROR_OUT_OF_HOST_MEMORY;
                    goto out;
                }

                loader_set_dispatch((void *)new_phys_devs[idx], inst->disp);
                new_phys_devs[idx]->this_icd_term = icd_phys_dev_array[icd_idx].this_icd_term;
                new_phys_devs[idx]->icd_index = (uint8_t)(icd_idx);
                new_phys_devs[idx]->phys_dev = icd_phys_dev_array[icd_idx].phys_devs[pd_idx];
            }
            idx++;
        }
    }

out:

    if (VK_SUCCESS != res) {
        if (NULL != new_phys_devs) {
            // We've encountered an error, so we should free the new buffers.
            for (uint32_t i = 0; i < inst->total_gpu_count; i++) {
                // If an OOM occurred inside the copying of the new physical devices into the existing array
                // will leave some of the old physical devices in the array which may have been copied into
                // the new array, leading to them being freed twice. To avoid this we just make sure to not
                // delete physical devices which were copied.
                bool found = false;
                if (NULL != inst->phys_devs_term) {
                    for (uint32_t old_idx = 0; old_idx < inst->phys_dev_count_term; old_idx++) {
                        if (new_phys_devs[i] == inst->phys_devs_term[old_idx]) {
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    loader_instance_heap_free(inst, new_phys_devs[i]);
                }
            }
            loader_instance_heap_free(inst, new_phys_devs);
        }
        inst->total_gpu_count = 0;
    } else {
        // Free everything that didn't carry over to the new array of
        // physical devices.  Everything else will have been copied over
        // to the new array.
        if (NULL != inst->phys_devs_term) {
            for (uint32_t cur_pd = 0; cur_pd < inst->phys_dev_count_term; cur_pd++) {
                bool found = false;
                for (uint32_t new_pd_idx = 0; new_pd_idx < inst->total_gpu_count; new_pd_idx++) {
                    if (inst->phys_devs_term[cur_pd] == new_phys_devs[new_pd_idx]) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    loader_instance_heap_free(inst, inst->phys_devs_term[cur_pd]);
                }
            }
            loader_instance_heap_free(inst, inst->phys_devs_term);
        }

        // Swap out old and new devices list
        inst->phys_dev_count_term = inst->total_gpu_count;
        inst->phys_devs_term = new_phys_devs;
    }

    if (sorted_phys_dev_array != NULL) {
        for (uint32_t i = 0; i < sorted_count; ++i) {
            if (sorted_phys_dev_array[i].device_count > 0 && sorted_phys_dev_array[i].physical_devices != NULL) {
                loader_instance_heap_free(inst, sorted_phys_dev_array[i].physical_devices);
            }
        }
        loader_instance_heap_free(inst, sorted_phys_dev_array);
    }

    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL terminator_EnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                                                   VkPhysicalDevice *pPhysicalDevices) {
    struct loader_instance *inst = (struct loader_instance *)instance;
    VkResult res = VK_SUCCESS;

    // Always call the setup loader terminator physical devices because they may
    // have changed at any point.
    res = setup_loader_term_phys_devs(inst);
    if (VK_SUCCESS != res) {
        goto out;
    }

    uint32_t copy_count = inst->total_gpu_count;
    if (NULL != pPhysicalDevices) {
        if (copy_count > *pPhysicalDeviceCount) {
            copy_count = *pPhysicalDeviceCount;
            res = VK_INCOMPLETE;
        }

        for (uint32_t i = 0; i < copy_count; i++) {
            pPhysicalDevices[i] = (VkPhysicalDevice)inst->phys_devs_term[i];
        }
    }

    *pPhysicalDeviceCount = copy_count;

out:

    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL terminator_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                             const char *pLayerName, uint32_t *pPropertyCount,
                                                                             VkExtensionProperties *pProperties) {
    struct loader_physical_device_term *phys_dev_term;

    struct loader_layer_list implicit_layer_list = {0};
    struct loader_extension_list all_exts = {0};
    struct loader_extension_list icd_exts = {0};

    // Any layer or trampoline wrapping should be removed at this point in time can just cast to the expected
    // type for VkPhysicalDevice.
    phys_dev_term = (struct loader_physical_device_term *)physicalDevice;

    // if we got here with a non-empty pLayerName, look up the extensions
    // from the json
    if (pLayerName != NULL && strlen(pLayerName) > 0) {
        uint32_t count;
        uint32_t copy_size;
        const struct loader_instance *inst = phys_dev_term->this_icd_term->this_instance;
        struct loader_device_extension_list *dev_ext_list = NULL;
        struct loader_device_extension_list local_ext_list;
        memset(&local_ext_list, 0, sizeof(local_ext_list));
        if (vk_string_validate(MaxLoaderStringLength, pLayerName) == VK_STRING_ERROR_NONE) {
            for (uint32_t i = 0; i < inst->instance_layer_list.count; i++) {
                struct loader_layer_properties *props = &inst->instance_layer_list.list[i];
                if (strcmp(props->info.layerName, pLayerName) == 0) {
                    dev_ext_list = &props->device_extension_list;
                }
            }

            count = (dev_ext_list == NULL) ? 0 : dev_ext_list->count;
            if (pProperties == NULL) {
                *pPropertyCount = count;
                loader_destroy_generic_list(inst, (struct loader_generic_list *)&local_ext_list);
                return VK_SUCCESS;
            }

            copy_size = *pPropertyCount < count ? *pPropertyCount : count;
            for (uint32_t i = 0; i < copy_size; i++) {
                memcpy(&pProperties[i], &dev_ext_list->list[i].props, sizeof(VkExtensionProperties));
            }
            *pPropertyCount = copy_size;

            loader_destroy_generic_list(inst, (struct loader_generic_list *)&local_ext_list);
            if (copy_size < count) {
                return VK_INCOMPLETE;
            }
        } else {
            loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                       "vkEnumerateDeviceExtensionProperties:  pLayerName is too long or is badly formed");
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }

        return VK_SUCCESS;
    }

    // This case is during the call down the instance chain with pLayerName == NULL
    struct loader_icd_term *icd_term = phys_dev_term->this_icd_term;
    uint32_t icd_ext_count = *pPropertyCount;
    VkExtensionProperties *icd_props_list = pProperties;
    VkResult res;

    if (NULL == icd_props_list) {
        // We need to find the count without duplicates. This requires querying the driver for the names of the extensions.
        // A small amount of storage is then needed to facilitate the de-duplication.
        res = icd_term->dispatch.EnumerateDeviceExtensionProperties(phys_dev_term->phys_dev, NULL, &icd_ext_count, NULL);
        if (res != VK_SUCCESS) {
            goto out;
        }
        if (icd_ext_count > 0) {
            icd_props_list = loader_instance_heap_alloc(icd_term->this_instance, sizeof(VkExtensionProperties) * icd_ext_count,
                                                        VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
            if (NULL == icd_props_list) {
                res = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto out;
            }
        }
    }

    // Get the available device extension count, and if pProperties is not NULL, the extensions as well
    res = icd_term->dispatch.EnumerateDeviceExtensionProperties(phys_dev_term->phys_dev, NULL, &icd_ext_count, icd_props_list);
    if (res != VK_SUCCESS) {
        goto out;
    }

    if (!loader_init_layer_list(icd_term->this_instance, &implicit_layer_list)) {
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }

    loader_add_implicit_layers(icd_term->this_instance, &implicit_layer_list, NULL, &icd_term->this_instance->instance_layer_list);

    // Initialize dev_extension list within the physicalDevice object
    res = loader_init_device_extensions(icd_term->this_instance, phys_dev_term, icd_ext_count, icd_props_list, &icd_exts);
    if (res != VK_SUCCESS) {
        goto out;
    }

    // We need to determine which implicit layers are active, and then add their extensions. This can't be cached as
    // it depends on results of environment variables (which can change).
    res = loader_add_to_ext_list(icd_term->this_instance, &all_exts, icd_exts.count, icd_exts.list);
    if (res != VK_SUCCESS) {
        goto out;
    }

    loader_add_implicit_layers(icd_term->this_instance, &implicit_layer_list, NULL, &icd_term->this_instance->instance_layer_list);

    for (uint32_t i = 0; i < implicit_layer_list.count; i++) {
        for (uint32_t j = 0; j < implicit_layer_list.list[i].device_extension_list.count; j++) {
            res = loader_add_to_ext_list(icd_term->this_instance, &all_exts, 1,
                                         &implicit_layer_list.list[i].device_extension_list.list[j].props);
            if (res != VK_SUCCESS) {
                goto out;
            }
        }
    }
    uint32_t capacity = *pPropertyCount;
    VkExtensionProperties *props = pProperties;

    res = VK_SUCCESS;
    if (NULL != pProperties) {
        for (uint32_t i = 0; i < all_exts.count && i < capacity; i++) {
            props[i] = all_exts.list[i];
        }

        // Wasn't enough space for the extensions, we did partial copy now return VK_INCOMPLETE
        if (capacity < all_exts.count) {
            res = VK_INCOMPLETE;
        } else {
            *pPropertyCount = all_exts.count;
        }
    } else {
        *pPropertyCount = all_exts.count;
    }

out:

    if (NULL != implicit_layer_list.list) {
        loader_destroy_generic_list(icd_term->this_instance, (struct loader_generic_list *)&implicit_layer_list);
    }
    if (NULL != all_exts.list) {
        loader_destroy_generic_list(icd_term->this_instance, (struct loader_generic_list *)&all_exts);
    }
    if (NULL != icd_exts.list) {
        loader_destroy_generic_list(icd_term->this_instance, (struct loader_generic_list *)&icd_exts);
    }
    if (NULL == pProperties && NULL != icd_props_list) {
        loader_instance_heap_free(icd_term->this_instance, icd_props_list);
    }
    return res;
}

VkStringErrorFlags vk_string_validate(const int max_length, const char *utf8) {
    VkStringErrorFlags result = VK_STRING_ERROR_NONE;
    int num_char_bytes = 0;
    int i, j;

    if (utf8 == NULL) {
        return VK_STRING_ERROR_NULL_PTR;
    }

    for (i = 0; i <= max_length; i++) {
        if (utf8[i] == 0) {
            break;
        } else if (i == max_length) {
            result |= VK_STRING_ERROR_LENGTH;
            break;
        } else if ((utf8[i] >= 0x20) && (utf8[i] < 0x7f)) {
            num_char_bytes = 0;
        } else if ((utf8[i] & UTF8_ONE_BYTE_MASK) == UTF8_ONE_BYTE_CODE) {
            num_char_bytes = 1;
        } else if ((utf8[i] & UTF8_TWO_BYTE_MASK) == UTF8_TWO_BYTE_CODE) {
            num_char_bytes = 2;
        } else if ((utf8[i] & UTF8_THREE_BYTE_MASK) == UTF8_THREE_BYTE_CODE) {
            num_char_bytes = 3;
        } else {
            result = VK_STRING_ERROR_BAD_DATA;
        }

        // Validate the following num_char_bytes of data
        for (j = 0; (j < num_char_bytes) && (i < max_length); j++) {
            if (++i == max_length) {
                result |= VK_STRING_ERROR_LENGTH;
                break;
            }
            if ((utf8[i] & UTF8_DATA_BYTE_MASK) != UTF8_DATA_BYTE_CODE) {
                result |= VK_STRING_ERROR_BAD_DATA;
            }
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL terminator_EnumerateInstanceVersion(const VkEnumerateInstanceVersionChain *chain,
                                                                   uint32_t *pApiVersion) {
    // NOTE: The Vulkan WG doesn't want us checking pApiVersion for NULL, but instead
    // prefers us crashing.
    *pApiVersion = VK_HEADER_VERSION_COMPLETE;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
terminator_EnumerateInstanceExtensionProperties(const VkEnumerateInstanceExtensionPropertiesChain *chain, const char *pLayerName,
                                                uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    struct loader_extension_list *global_ext_list = NULL;
    struct loader_layer_list instance_layers;
    struct loader_extension_list local_ext_list;
    struct loader_icd_tramp_list icd_tramp_list;
    uint32_t copy_size;
    VkResult res = VK_SUCCESS;

    memset(&local_ext_list, 0, sizeof(local_ext_list));
    memset(&instance_layers, 0, sizeof(instance_layers));
    memset(&icd_tramp_list, 0, sizeof(icd_tramp_list));

    // Get layer libraries if needed
    if (pLayerName && strlen(pLayerName) != 0) {
        if (vk_string_validate(MaxLoaderStringLength, pLayerName) != VK_STRING_ERROR_NONE) {
            assert(VK_FALSE && "vkEnumerateInstanceExtensionProperties: pLayerName is too long or is badly formed");
            res = VK_ERROR_EXTENSION_NOT_PRESENT;
            goto out;
        }

        loader_scan_for_layers(NULL, &instance_layers);
        for (uint32_t i = 0; i < instance_layers.count; i++) {
            struct loader_layer_properties *props = &instance_layers.list[i];
            if (strcmp(props->info.layerName, pLayerName) == 0) {
                global_ext_list = &props->instance_extension_list;
                break;
            }
        }
    } else {
        // Preload ICD libraries so subsequent calls to EnumerateInstanceExtensionProperties don't have to load them
        loader_preload_icds();

        // Scan/discover all ICD libraries
        res = loader_icd_scan(NULL, &icd_tramp_list);
        // EnumerateInstanceExtensionProperties can't return anything other than OOM or VK_ERROR_LAYER_NOT_PRESENT
        if ((VK_SUCCESS != res && icd_tramp_list.count > 0) || res == VK_ERROR_OUT_OF_HOST_MEMORY) {
            goto out;
        }
        // Get extensions from all ICD's, merge so no duplicates
        res = loader_get_icd_loader_instance_extensions(NULL, &icd_tramp_list, &local_ext_list);
        if (VK_SUCCESS != res) {
            goto out;
        }
        loader_scanned_icd_clear(NULL, &icd_tramp_list);

        // Append enabled implicit layers.
        loader_scan_for_implicit_layers(NULL, &instance_layers);
        for (uint32_t i = 0; i < instance_layers.count; i++) {
            if (!loader_implicit_layer_is_enabled(NULL, &instance_layers.list[i])) {
                continue;
            }
            struct loader_extension_list *ext_list = &instance_layers.list[i].instance_extension_list;
            loader_add_to_ext_list(NULL, &local_ext_list, ext_list->count, ext_list->list);
        }

        global_ext_list = &local_ext_list;
    }

    if (global_ext_list == NULL) {
        res = VK_ERROR_LAYER_NOT_PRESENT;
        goto out;
    }

    if (pProperties == NULL) {
        *pPropertyCount = global_ext_list->count;
        goto out;
    }

    copy_size = *pPropertyCount < global_ext_list->count ? *pPropertyCount : global_ext_list->count;
    for (uint32_t i = 0; i < copy_size; i++) {
        memcpy(&pProperties[i], &global_ext_list->list[i], sizeof(VkExtensionProperties));
    }
    *pPropertyCount = copy_size;

    if (copy_size < global_ext_list->count) {
        res = VK_INCOMPLETE;
        goto out;
    }

out:
    loader_destroy_generic_list(NULL, (struct loader_generic_list *)&icd_tramp_list);
    loader_destroy_generic_list(NULL, (struct loader_generic_list *)&local_ext_list);
    loader_delete_layer_list_and_properties(NULL, &instance_layers);
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL terminator_EnumerateInstanceLayerProperties(const VkEnumerateInstanceLayerPropertiesChain *chain,
                                                                           uint32_t *pPropertyCount,
                                                                           VkLayerProperties *pProperties) {
    VkResult result = VK_SUCCESS;
    struct loader_layer_list instance_layer_list;

    LOADER_PLATFORM_THREAD_ONCE(&once_init, loader_initialize);

    uint32_t copy_size;

    // Get layer libraries
    memset(&instance_layer_list, 0, sizeof(instance_layer_list));
    loader_scan_for_layers(NULL, &instance_layer_list);

    if (pProperties == NULL) {
        *pPropertyCount = instance_layer_list.count;
        goto out;
    }

    copy_size = (*pPropertyCount < instance_layer_list.count) ? *pPropertyCount : instance_layer_list.count;
    for (uint32_t i = 0; i < copy_size; i++) {
        memcpy(&pProperties[i], &instance_layer_list.list[i].info, sizeof(VkLayerProperties));
    }

    *pPropertyCount = copy_size;

    if (copy_size < instance_layer_list.count) {
        result = VK_INCOMPLETE;
        goto out;
    }

out:

    loader_delete_layer_list_and_properties(NULL, &instance_layer_list);
    return result;
}

// ---- Vulkan Core 1.1 terminators

VkResult setup_loader_term_phys_dev_groups(struct loader_instance *inst) {
    VkResult res = VK_SUCCESS;
    struct loader_icd_term *icd_term;
    uint32_t total_count = 0;
    uint32_t cur_icd_group_count = 0;
    VkPhysicalDeviceGroupPropertiesKHR **new_phys_dev_groups = NULL;
    struct loader_physical_device_group_term *local_phys_dev_groups = NULL;
    bool *local_phys_dev_group_sorted = NULL;
    PFN_vkEnumeratePhysicalDeviceGroups fpEnumeratePhysicalDeviceGroups = NULL;
    struct LoaderSortedPhysicalDevice *sorted_phys_dev_array = NULL;
    uint32_t sorted_count = 0;
    uint32_t icd_idx = 0;

    if (0 == inst->phys_dev_count_term) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "setup_loader_term_phys_dev_groups:  Loader failed to setup physical device terminator info before calling "
                   "\'EnumeratePhysicalDeviceGroups\'.");
        assert(false);
        res = VK_ERROR_INITIALIZATION_FAILED;
        goto out;
    }

    // For each ICD, query the number of physical device groups, and then get an
    // internal value for those physical devices.
    icd_term = inst->icd_terms;
    for (icd_idx = 0; NULL != icd_term; icd_term = icd_term->next, icd_idx++) {
        // Get the function pointer to use to call into the ICD. This could be the core or KHR version
        if (inst->enabled_known_extensions.khr_device_group_creation) {
            fpEnumeratePhysicalDeviceGroups = icd_term->dispatch.EnumeratePhysicalDeviceGroupsKHR;
        } else {
            fpEnumeratePhysicalDeviceGroups = icd_term->dispatch.EnumeratePhysicalDeviceGroups;
        }

        cur_icd_group_count = 0;
        if (NULL == fpEnumeratePhysicalDeviceGroups) {
            // Treat each ICD's GPU as it's own group if the extension isn't supported
            res = icd_term->dispatch.EnumeratePhysicalDevices(icd_term->instance, &cur_icd_group_count, NULL);
            if (res != VK_SUCCESS) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "setup_loader_term_phys_dev_groups:  Failed during dispatch call of "
                           "\'EnumeratePhysicalDevices\' to ICD %d to get plain phys dev count.",
                           icd_idx);
                goto out;
            }
        } else {
            // Query the actual group info
            res = fpEnumeratePhysicalDeviceGroups(icd_term->instance, &cur_icd_group_count, NULL);
            if (res != VK_SUCCESS) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "setup_loader_term_phys_dev_groups:  Failed during dispatch call of "
                           "\'EnumeratePhysicalDeviceGroups\' to ICD %d to get count.",
                           icd_idx);
                goto out;
            }
        }
        total_count += cur_icd_group_count;
    }

    if (total_count == 0) {
        loader_log(inst, VK_DEBUG_REPORT_INFORMATION_BIT_EXT, 0,
                   "setupLoaderTermPhysDevGroups:  Did not detect any GPU Groups"
                   " in the current config");
        goto out;
    }

    // Create an array for the new physical device groups, which will be stored
    // in the instance for the Terminator code.
    new_phys_dev_groups = (VkPhysicalDeviceGroupProperties **)loader_instance_heap_alloc(
        inst, total_count * sizeof(VkPhysicalDeviceGroupProperties *), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    if (NULL == new_phys_dev_groups) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "setup_loader_term_phys_dev_groups:  Failed to allocate new physical device group array of size %d",
                   total_count);
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }
    memset(new_phys_dev_groups, 0, total_count * sizeof(VkPhysicalDeviceGroupProperties *));

    // Create a temporary array (on the stack) to keep track of the
    // returned VkPhysicalDevice values.
    local_phys_dev_groups = loader_stack_alloc(sizeof(struct loader_physical_device_group_term) * total_count);
    local_phys_dev_group_sorted = loader_stack_alloc(sizeof(bool) * total_count);
    if (NULL == local_phys_dev_groups || NULL == local_phys_dev_group_sorted) {
        loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                   "setup_loader_term_phys_dev_groups:  Failed to allocate local physical device group array of size %d",
                   total_count);
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto out;
    }
    // Initialize the memory to something valid
    memset(local_phys_dev_groups, 0, sizeof(struct loader_physical_device_group_term) * total_count);
    memset(local_phys_dev_group_sorted, 0, sizeof(bool) * total_count);
    for (uint32_t group = 0; group < total_count; group++) {
        local_phys_dev_groups[group].group_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES_KHR;
        local_phys_dev_groups[group].group_props.pNext = NULL;
        local_phys_dev_groups[group].group_props.subsetAllocation = false;
    }

#if defined(_WIN32)
    // Get the physical devices supported by platform sorting mechanism into a separate list
    res = windows_read_sorted_physical_devices(inst, &sorted_phys_dev_array, &sorted_count);
    if (VK_SUCCESS != res) {
        goto out;
    }
#endif

    cur_icd_group_count = 0;
    icd_term = inst->icd_terms;
    for (icd_idx = 0; NULL != icd_term; icd_term = icd_term->next, icd_idx++) {
        uint32_t count_this_time = total_count - cur_icd_group_count;

        // Check if this group can be sorted
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        bool icd_sorted = sorted_count && (icd_term->scanned_icd->EnumerateAdapterPhysicalDevices != NULL);
#else
        bool icd_sorted = false;
#endif

        // Get the function pointer to use to call into the ICD. This could be the core or KHR version
        if (inst->enabled_known_extensions.khr_device_group_creation) {
            fpEnumeratePhysicalDeviceGroups = icd_term->dispatch.EnumeratePhysicalDeviceGroupsKHR;
        } else {
            fpEnumeratePhysicalDeviceGroups = icd_term->dispatch.EnumeratePhysicalDeviceGroups;
        }

        if (NULL == fpEnumeratePhysicalDeviceGroups) {
            VkPhysicalDevice *phys_dev_array = loader_stack_alloc(sizeof(VkPhysicalDevice) * count_this_time);
            if (NULL == phys_dev_array) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "setup_loader_term_phys_dev_groups:  Failed to allocate local physical device array of size %d",
                           count_this_time);
                res = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto out;
            }

            res = icd_term->dispatch.EnumeratePhysicalDevices(icd_term->instance, &count_this_time, phys_dev_array);
            if (res != VK_SUCCESS) {
                loader_log(
                    inst, VULKAN_LOADER_ERROR_BIT, 0,
                    "setup_loader_term_phys_dev_groups:  Failed during dispatch call of \'EnumeratePhysicalDevices\' to ICD %d "
                    "to get plain phys dev count.",
                    icd_idx);
                goto out;
            }

            // Add each GPU as it's own group
            for (uint32_t indiv_gpu = 0; indiv_gpu < count_this_time; indiv_gpu++) {
                local_phys_dev_groups[indiv_gpu + cur_icd_group_count].this_icd_term = icd_term;
                local_phys_dev_groups[indiv_gpu + cur_icd_group_count].icd_index = icd_idx;
                local_phys_dev_groups[indiv_gpu + cur_icd_group_count].group_props.physicalDeviceCount = 1;
                local_phys_dev_groups[indiv_gpu + cur_icd_group_count].group_props.physicalDevices[0] = phys_dev_array[indiv_gpu];
                local_phys_dev_group_sorted[indiv_gpu + cur_icd_group_count] = icd_sorted;
            }

        } else {
            res = fpEnumeratePhysicalDeviceGroups(icd_term->instance, &count_this_time,
                                                  &local_phys_dev_groups[cur_icd_group_count].group_props);
            for (uint32_t group = 0; group < count_this_time; ++group) {
                local_phys_dev_group_sorted[group + cur_icd_group_count] = icd_sorted;
                local_phys_dev_groups[group].this_icd_term = icd_term;
                local_phys_dev_groups[group].icd_index = icd_idx;
            }
            if (VK_SUCCESS != res) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "setup_loader_term_phys_dev_groups:  Failed during dispatch call of "
                           "\'EnumeratePhysicalDeviceGroups\' to ICD %d to get content.",
                           icd_idx);
                goto out;
            }
        }

        cur_icd_group_count += count_this_time;
    }

#ifdef LOADER_ENABLE_LINUX_SORT
    if (is_linux_sort_enabled(inst)) {
        // Get the physical devices supported by platform sorting mechanism into a separate list
        res = linux_read_sorted_physical_device_groups(inst, total_count, local_phys_dev_groups);
    }
#endif  // LOADER_ENABLE_LINUX_SORT

    // Replace all the physical device IDs with the proper loader values
    for (uint32_t group = 0; group < total_count; group++) {
        for (uint32_t group_gpu = 0; group_gpu < local_phys_dev_groups[group].group_props.physicalDeviceCount; group_gpu++) {
            bool found = false;
            for (uint32_t term_gpu = 0; term_gpu < inst->phys_dev_count_term; term_gpu++) {
                if (local_phys_dev_groups[group].group_props.physicalDevices[group_gpu] ==
                    inst->phys_devs_term[term_gpu]->phys_dev) {
                    local_phys_dev_groups[group].group_props.physicalDevices[group_gpu] =
                        (VkPhysicalDevice)inst->phys_devs_term[term_gpu];
                    found = true;
                    break;
                }
            }
            if (!found) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "setup_loader_term_phys_dev_groups:  Failed to find GPU %d in group %d returned by "
                           "\'EnumeratePhysicalDeviceGroups\' in list returned by \'EnumeratePhysicalDevices\'",
                           group_gpu, group);
                res = VK_ERROR_INITIALIZATION_FAILED;
                goto out;
            }
        }
    }

    uint32_t idx = 0;

#if defined(_WIN32)
    // Copy over everything found through sorted enumeration
    for (uint32_t i = 0; i < sorted_count; ++i) {
        // Find the VkPhysicalDeviceGroupProperties object in local_phys_dev_groups
        VkPhysicalDeviceGroupProperties *group_properties = NULL;
        for (uint32_t group = 0; group < total_count; group++) {
            if (sorted_phys_dev_array[i].device_count != local_phys_dev_groups[group].group_props.physicalDeviceCount) {
                continue;
            }

            bool match = true;
            for (uint32_t group_gpu = 0; group_gpu < local_phys_dev_groups[group].group_props.physicalDeviceCount; group_gpu++) {
                if (sorted_phys_dev_array[i].physical_devices[group_gpu] !=
                    ((struct loader_physical_device_term *)local_phys_dev_groups[group].group_props.physicalDevices[group_gpu])
                        ->phys_dev) {
                    match = false;
                    break;
                }
            }

            if (match) {
                group_properties = &local_phys_dev_groups[group].group_props;
            }
        }

        // Check if this physical device group with the same contents is already in the old buffer
        for (uint32_t old_idx = 0; old_idx < inst->phys_dev_group_count_term; old_idx++) {
            if (NULL != group_properties &&
                group_properties->physicalDeviceCount == inst->phys_dev_groups_term[old_idx]->physicalDeviceCount) {
                bool found_all_gpus = true;
                for (uint32_t old_gpu = 0; old_gpu < inst->phys_dev_groups_term[old_idx]->physicalDeviceCount; old_gpu++) {
                    bool found_gpu = false;
                    for (uint32_t new_gpu = 0; new_gpu < group_properties->physicalDeviceCount; new_gpu++) {
                        if (group_properties->physicalDevices[new_gpu] ==
                            inst->phys_dev_groups_term[old_idx]->physicalDevices[old_gpu]) {
                            found_gpu = true;
                            break;
                        }
                    }

                    if (!found_gpu) {
                        found_all_gpus = false;
                        break;
                    }
                }
                if (!found_all_gpus) {
                    continue;
                } else {
                    new_phys_dev_groups[idx] = inst->phys_dev_groups_term[old_idx];
                    break;
                }
            }
        }

        // If this physical device group isn't in the old buffer, create it
        if (group_properties != NULL && NULL == new_phys_dev_groups[idx]) {
            new_phys_dev_groups[idx] = (VkPhysicalDeviceGroupPropertiesKHR *)loader_instance_heap_alloc(
                inst, sizeof(VkPhysicalDeviceGroupPropertiesKHR), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
            if (NULL == new_phys_dev_groups[idx]) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "setup_loader_term_phys_dev_groups:  Failed to allocate physical device group Terminator object %d",
                           idx);
                total_count = idx;
                res = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto out;
            }
            memcpy(new_phys_dev_groups[idx], group_properties, sizeof(VkPhysicalDeviceGroupPropertiesKHR));
        }

        ++idx;
    }
#endif

    // Copy or create everything to fill the new array of physical device groups
    for (uint32_t new_idx = 0; new_idx < total_count; new_idx++) {
        // Skip groups which have been included through sorting
        if (local_phys_dev_group_sorted[new_idx] || local_phys_dev_groups[new_idx].group_props.physicalDeviceCount == 0) {
            continue;
        }

        // Check if this physical device group with the same contents is already in the old buffer
        for (uint32_t old_idx = 0; old_idx < inst->phys_dev_group_count_term; old_idx++) {
            if (local_phys_dev_groups[new_idx].group_props.physicalDeviceCount ==
                inst->phys_dev_groups_term[old_idx]->physicalDeviceCount) {
                bool found_all_gpus = true;
                for (uint32_t old_gpu = 0; old_gpu < inst->phys_dev_groups_term[old_idx]->physicalDeviceCount; old_gpu++) {
                    bool found_gpu = false;
                    for (uint32_t new_gpu = 0; new_gpu < local_phys_dev_groups[new_idx].group_props.physicalDeviceCount;
                         new_gpu++) {
                        if (local_phys_dev_groups[new_idx].group_props.physicalDevices[new_gpu] ==
                            inst->phys_dev_groups_term[old_idx]->physicalDevices[old_gpu]) {
                            found_gpu = true;
                            break;
                        }
                    }

                    if (!found_gpu) {
                        found_all_gpus = false;
                        break;
                    }
                }
                if (!found_all_gpus) {
                    continue;
                } else {
                    new_phys_dev_groups[idx] = inst->phys_dev_groups_term[old_idx];
                    break;
                }
            }
        }

        // If this physical device group isn't in the old buffer, create it
        if (NULL == new_phys_dev_groups[idx]) {
            new_phys_dev_groups[idx] = (VkPhysicalDeviceGroupPropertiesKHR *)loader_instance_heap_alloc(
                inst, sizeof(VkPhysicalDeviceGroupPropertiesKHR), VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
            if (NULL == new_phys_dev_groups[idx]) {
                loader_log(inst, VULKAN_LOADER_ERROR_BIT, 0,
                           "setup_loader_term_phys_dev_groups:  Failed to allocate physical device group Terminator object %d",
                           idx);
                total_count = idx;
                res = VK_ERROR_OUT_OF_HOST_MEMORY;
                goto out;
            }
            memcpy(new_phys_dev_groups[idx], &local_phys_dev_groups[new_idx].group_props,
                   sizeof(VkPhysicalDeviceGroupPropertiesKHR));
        }

        ++idx;
    }

out:

    if (VK_SUCCESS != res) {
        if (NULL != new_phys_dev_groups) {
            for (uint32_t i = 0; i < total_count; i++) {
                loader_instance_heap_free(inst, new_phys_dev_groups[i]);
            }
            loader_instance_heap_free(inst, new_phys_dev_groups);
        }
    } else {
        // Free everything that didn't carry over to the new array of
        // physical device groups
        if (NULL != inst->phys_dev_groups_term) {
            for (uint32_t i = 0; i < inst->phys_dev_group_count_term; i++) {
                bool found = false;
                for (uint32_t j = 0; j < total_count; j++) {
                    if (inst->phys_dev_groups_term[i] == new_phys_dev_groups[j]) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    loader_instance_heap_free(inst, inst->phys_dev_groups_term[i]);
                }
            }
            loader_instance_heap_free(inst, inst->phys_dev_groups_term);
        }

        // Swap in the new physical device group list
        inst->phys_dev_group_count_term = total_count;
        inst->phys_dev_groups_term = new_phys_dev_groups;
    }

    if (sorted_phys_dev_array != NULL) {
        for (uint32_t i = 0; i < sorted_count; ++i) {
            if (sorted_phys_dev_array[i].device_count > 0 && sorted_phys_dev_array[i].physical_devices != NULL) {
                loader_instance_heap_free(inst, sorted_phys_dev_array[i].physical_devices);
            }
        }
        loader_instance_heap_free(inst, sorted_phys_dev_array);
    }

    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL terminator_EnumeratePhysicalDeviceGroups(
    VkInstance instance, uint32_t *pPhysicalDeviceGroupCount, VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties) {
    struct loader_instance *inst = (struct loader_instance *)instance;
    VkResult res = VK_SUCCESS;

    // Always call the setup loader terminator physical device groups because they may
    // have changed at any point.
    res = setup_loader_term_phys_dev_groups(inst);
    if (VK_SUCCESS != res) {
        goto out;
    }

    uint32_t copy_count = inst->phys_dev_group_count_term;
    if (NULL != pPhysicalDeviceGroupProperties) {
        if (copy_count > *pPhysicalDeviceGroupCount) {
            copy_count = *pPhysicalDeviceGroupCount;
            res = VK_INCOMPLETE;
        }

        for (uint32_t i = 0; i < copy_count; i++) {
            memcpy(&pPhysicalDeviceGroupProperties[i], inst->phys_dev_groups_term[i], sizeof(VkPhysicalDeviceGroupPropertiesKHR));
        }
    }

    *pPhysicalDeviceGroupCount = copy_count;

out:

    return res;
}
