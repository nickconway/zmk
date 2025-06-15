/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_triplex

#include <device.h>
#include <drivers/behavior.h>
#include <logging/log.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/hid.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define ZMK_BHV_MAX_ACTIVE_TRIPLEX 10

struct behavior_triplex_config {
    int32_t shared_key_positions_len;
    int32_t shared_layers_len;
    struct zmk_behavior_binding *behaviors;
    int32_t shared_layers[32];
    int32_t shared_key_positions[];
};

struct active_triplex {
    bool is_active;
    bool is_pressed;
    bool first_press;
    uint32_t position;
    const struct behavior_triplex_config *config;
};

struct active_triplex active_triplexes[ZMK_BHV_MAX_ACTIVE_TRIPLEX] = {};

static struct active_triplex *find_triplex(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_TRIPLEX; i++) {
        if (active_triplexes[i].position == position && active_triplexes[i].is_active) {
            return &active_triplexes[i];
        }
    }
    return NULL;
}

static int new_triplex(uint32_t position, const struct behavior_triplex_config *config,
                       struct active_triplex **triplex) {
    for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_TRIPLEX; i++) {
        struct active_triplex *const ref_triplex = &active_triplexes[i];
        if (!ref_triplex->is_active) {
            ref_triplex->position = position;
            ref_triplex->config = config;
            ref_triplex->is_active = true;
            ref_triplex->is_pressed = false;
            ref_triplex->first_press = true;
            *triplex = ref_triplex;
            return 0;
        }
    }
    return -ENOMEM;
}

static bool is_other_key_shared(struct active_triplex *triplex, int32_t position) {
    for (int i = 0; i < triplex->config->shared_key_positions_len; i++) {
        if (triplex->config->shared_key_positions[i] == position) {
            return true;
        }
    }
    return false;
}

static bool is_layer_shared(struct active_triplex *triplex, int32_t layer) {
    for (int i = 0; i < triplex->config->shared_layers_len; i++) {
        if (triplex->config->shared_layers[i] == layer) {
            return true;
        }
    }
    return false;
}

static int on_triplex_binding_pressed(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct behavior_triplex_config *cfg = dev->config;
    struct active_triplex *triplex;
    triplex = find_triplex(event.position);
    if (triplex == NULL) {
        if (new_triplex(event.position, cfg, &triplex) == -ENOMEM) {
            LOG_ERR("Unable to create new triplex. Insufficient space in active_triplexes[].");
            return ZMK_BEHAVIOR_OPAQUE;
        }
        LOG_DBG("%d created new triplex", event.position);
    }
    LOG_DBG("%d triplex pressed", event.position);
    triplex->is_pressed = true;
    if (triplex->first_press) {
        behavior_keymap_binding_pressed(&cfg->behaviors[0], event);
        behavior_keymap_binding_released(&cfg->behaviors[0], event);
        triplex->first_press = false;
    }
    behavior_keymap_binding_pressed(&cfg->behaviors[1], event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_triplex_binding_released(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event) {
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct behavior_triplex_config *cfg = dev->config;
    LOG_DBG("%d triplex keybind released", event.position);
    struct active_triplex *triplex = find_triplex(event.position);
    if (triplex == NULL) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    triplex->is_pressed = false;
    behavior_keymap_binding_released(&cfg->behaviors[1], event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int triplex_init(const struct device *dev) { return 0; }

static const struct behavior_driver_api behavior_triplex_driver_api = {
    .binding_pressed = on_triplex_binding_pressed,
    .binding_released = on_triplex_binding_released,
};

static int triplex_position_state_changed_listener(const zmk_event_t *eh);
static int triplex_layer_state_changed_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_triplex, triplex_position_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_triplex, zmk_position_state_changed);

ZMK_LISTENER(behavior_triplex2, triplex_layer_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_triplex2, zmk_layer_state_changed);

static int triplex_position_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_TRIPLEX; i++) {
        struct active_triplex *triplex = &active_triplexes[i];
        if (!triplex->is_active) {
            continue;
        }
        if (triplex->position == ev->position) {
            continue;
        }
        if (!is_other_key_shared(triplex, ev->position)) {
            LOG_DBG("Triplex interrupted, ending at %d %d", triplex->position, ev->position);
            triplex->is_active = false;
            struct zmk_behavior_binding_event event = {.position = triplex->position,
                                                       .timestamp = k_uptime_get()};
            if (triplex->is_pressed) {
                behavior_keymap_binding_released(&triplex->config->behaviors[1], event);
            }
            behavior_keymap_binding_pressed(&triplex->config->behaviors[2], event);
            behavior_keymap_binding_released(&triplex->config->behaviors[2], event);
            return ZMK_EV_EVENT_BUBBLE;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int triplex_layer_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_TRIPLEX; i++) {
        struct active_triplex *triplex = &active_triplexes[i];
        if (!triplex->is_active) {
            continue;
        }
        if (!is_layer_shared(triplex, ev->layer)) {
            LOG_DBG("Triplex layer changed, ending at %d %d", triplex->position, ev->layer);
            triplex->is_active = false;
            struct zmk_behavior_binding_event event = {.position = triplex->position,
                                                       .timestamp = k_uptime_get()};
            if (triplex->is_pressed) {
                behavior_keymap_binding_released(&triplex->config->behaviors[1], event);
            }
            behavior_keymap_binding_pressed(&triplex->config->behaviors[2], event);
            behavior_keymap_binding_released(&triplex->config->behaviors[2], event);
            return ZMK_EV_EVENT_BUBBLE;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

#define _TRANSFORM_ENTRY(idx, node) ZMK_KEYMAP_EXTRACT_BINDING(idx, node),

#define TRANSFORMED_BINDINGS(node)                                                                 \
    {UTIL_LISTIFY(DT_INST_PROP_LEN(node, bindings), _TRANSFORM_ENTRY, DT_DRV_INST(node))}

#define TRIPLEX_INST(n)                                                                            \
    static struct zmk_behavior_binding                                                             \
        behavior_triplex_config_##n##_bindings[DT_INST_PROP_LEN(n, bindings)] =                    \
            TRANSFORMED_BINDINGS(n);                                                               \
    static struct behavior_triplex_config behavior_triplex_config_##n = {                          \
        .shared_key_positions = DT_INST_PROP(n, shared_key_positions),                             \
        .shared_key_positions_len = DT_INST_PROP_LEN(n, shared_key_positions),                     \
        .shared_layers = DT_INST_PROP(n, shared_layers),                                           \
        .shared_layers_len = DT_INST_PROP_LEN(n, shared_layers),                                   \
        .behaviors = behavior_triplex_config_##n##_bindings};                                      \
    DEVICE_DT_INST_DEFINE(n, triplex_init, NULL, NULL, &behavior_triplex_config_##n, APPLICATION,  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_triplex_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TRIPLEX_INST)
