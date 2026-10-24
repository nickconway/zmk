/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_gestures

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/virtual_key_position.h>

struct gesture_config {
    uint8_t fingers;
    uint8_t index;
    size_t size;

    const uint16_t *codes;
    const struct zmk_behavior_binding *bindings;
    const uint8_t invoke_every_ms;
};

struct gesture_data {
    struct k_mutex lock;
    int64_t last_invoked_timestamp;
};

static int gesture_tap_binding(const struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    int ret = zmk_behavior_invoke_binding(binding, event, true);

    if (ret < 0) {
        return ret;
    }

    ret = zmk_behavior_invoke_binding(binding, event, false);

    if (ret < 0) {
        return ret;
    }

    return 0;
};

static int gestures_handle_event(const struct device *dev, struct input_event *event,
                                 uint32_t param1, uint32_t param2,
                                 struct zmk_input_processor_state *state) {
    const struct gesture_config *cfg = dev->config;
    struct gesture_data *data = (struct gesture_data *)dev->data;

    if (event->value == 0 || event->value != cfg->fingers) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    for (size_t i = 0; i < cfg->size; i++) {
        if (cfg->codes[i] == event->code) {
            struct zmk_behavior_binding_event behavior_event = {
                .position = ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(
                    state->input_device_index, cfg->index),
                .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
                .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
            };

            const struct zmk_behavior_binding *binding = &cfg->bindings[i];
            LOG_DBG("%lld - %lld (%lld) > %d", k_uptime_get(), data->last_invoked_timestamp,
                    (k_uptime_get() - data->last_invoked_timestamp), cfg->invoke_every_ms);

            if ((k_uptime_get() - data->last_invoked_timestamp) > cfg->invoke_every_ms) {
                int ret = k_mutex_lock(&data->lock, K_FOREVER);

                if (ret < 0) {
                    LOG_ERR("Error locking for updating %d", ret);
                    return ZMK_INPUT_PROC_STOP;
                }

                LOG_DBG("invoking %s (%d %d) from %d for position %d with %d listeners",
                        binding->behavior_dev, binding->param1, binding->param2, event->value,
                        behavior_event.position, ZMK_INPUT_LISTENERS_LEN);

                data->last_invoked_timestamp = k_uptime_get();
                ret = gesture_tap_binding(binding, behavior_event);
                k_mutex_unlock(&data->lock);

                if (ret < 0) {
                    return ret;
                }

                return ZMK_INPUT_PROC_STOP;
            }

            return ZMK_INPUT_PROC_CONTINUE;
        }
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api gestures_driver_api = {
    .handle_event = gestures_handle_event,
};

static int ip_gestures_init(const struct device *dev) { return 0; }

#define ip_gestures_INST(n)                                                                        \
    static struct gesture_data gestures_data_##n = {};                                             \
    static const uint16_t ip_gestures_codes_##n[] = DT_INST_PROP(n, codes);                        \
    static const struct zmk_behavior_binding ip_gestures_bindings_##n[] = {                        \
        LISTIFY(DT_INST_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(n))}; \
    BUILD_ASSERT(ARRAY_SIZE(ip_gestures_codes_##n) == ARRAY_SIZE(ip_gestures_bindings_##n),        \
                 "bindings needs to be the same length as codes");                                 \
    static const struct gesture_config gestures_config_##n = {                                     \
        .index = n,                                                                                \
        .size = DT_INST_PROP_LEN(n, codes),                                                        \
        .codes = ip_gestures_codes_##n,                                                            \
        .bindings = ip_gestures_bindings_##n,                                                      \
        .invoke_every_ms = DT_INST_PROP_OR(n, invoke_every_ms, 0)};                                \
    DEVICE_DT_INST_DEFINE(n, &ip_gestures_init, NULL, &gestures_data_##n, &gestures_config_##n,    \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &gestures_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ip_gestures_INST)
