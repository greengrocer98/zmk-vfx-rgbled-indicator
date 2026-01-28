#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/keymap.h>
#include <zmk/workqueue.h>

#include <zmk_vfx_rgbled_indicator/indicator.h>

#if IS_ENABLED(CONFIG_REPORT_ATTR)
extern struct k_msgq attr_msgq;
static struct attr_report report;
#endif

#define LED_COUNT 3

// Define stack size and priority for animation workqueue
#define ANIMATION_WORK_Q_STACK_SIZE 1024
#define ANIMATION_WORK_Q_PRIORITY 5

#ifndef LED_BRIGHTNESS_MAX
#define LED_BRIGHTNESS_MAX 100
#endif

#define PWM_STEP_PERIOD_MS 10

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led0)),
             "An alias for a first LED is not found for VFX_INDICATOR");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led1)),
             "An alias for a second LED is not found for VFX_INDICATOR");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led2)),
             "An alias for a third LED is not found for VFX_INDICATOR");

#ifdef CONFIG_VFX_LDO_PIN
static const struct gpio_dt_spec ldo_pin = GPIO_DT_SPEC_GET(DT_ALIAS(ldo), gpios);
#endif

#ifdef CONFIG_VFX_CHRG_PIN
static const struct gpio_dt_spec chrg_pin = GPIO_DT_SPEC_GET(DT_ALIAS(chrg), gpios);
static struct gpio_callback chrg_cb;
#endif

// GPIO-based LED device and indices of LEDs inside its DT node
static const struct device *led_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight));
static const uint8_t led_idx[] = {DT_NODE_CHILD_IDX(DT_ALIAS(led0)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(led1)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(led2))};

enum animation_state
{
    CHARGING,
    DISCHARGED,
    IDLE,
};

enum animation_event
{
    START_BATTERY_STATUS,
    START_CONNECTION_STATUS,
    START_CHARGING,
    START_DISCHARGED,
    START_ATTR_REPORT,
    STOP_ANIMATION,
};

struct anim_ctx
{
    struct k_work_delayable work;
    atomic_t next_gen;
    uint32_t current_gen;
};

static struct anim_ctx anim;
static enum animation_state fmt_state;
static enum animation_event fmt_event;
#if IS_ENABLED(CONFIG_REPORT_ATTR)
static struct k_thread msgq_thread_data;
K_THREAD_STACK_DEFINE(msgq_thread_stack, 1024);
#endif

// Define stack area for animation workqueue
K_THREAD_STACK_DEFINE(animation_work_q_stack, ANIMATION_WORK_Q_STACK_SIZE);
static struct k_work_q animation_work_q;

static void leds_off()
{
    for (int i = 0; i < LED_COUNT; i++)
    {
        led_off(led_dev, led_idx[i]);
    }
}

static void pwm_leds_bitmask(uint8_t mask, uint16_t step_period_ms)
{
    for (int brightness = 0; brightness <= LED_BRIGHTNESS_MAX; brightness++)
    {
        if (anim.current_gen != atomic_get(&anim.next_gen))
        {
            leds_off();
            return;
        }
        for (int i = 0; i < LED_COUNT; i++)
        {
            if (mask & (1 << i))
            {
                led_set_brightness(led_dev, led_idx[i], brightness);
            }
        }
        k_sleep(K_MSEC(step_period_ms));
    }
    for (int brightness = LED_BRIGHTNESS_MAX - 1; brightness >= 0; brightness--)
    {
        if (anim.current_gen != atomic_get(&anim.next_gen))
        {
            leds_off();
            return;
        }
        for (int i = 0; i < LED_COUNT; i++)
        {
            if (mask & (1 << i))
            {
                led_set_brightness(led_dev, led_idx[i], brightness);
            }
        }
        k_sleep(K_MSEC(step_period_ms));
    }
    leds_off();
}

static void battery_status_animation()
{
    uint8_t battery_level = zmk_battery_state_of_charge();
#ifdef CONFIG_VFX_LDO_PIN
    gpio_pin_set_dt(&ldo_pin, 1);
#endif
    LOG_DBG("Battery level: %d%%", battery_level);
    if (battery_level > CONFIG_VFX_BATTERY_LEVEL_HIGH)
    {
        pwm_leds_bitmask(0b010, 5);
    }
    else if (battery_level > CONFIG_VFX_BATTERY_LEVEL_MID)
    {
        pwm_leds_bitmask(0b011, 5);
    }
    else if (battery_level > CONFIG_VFX_BATTERY_LEVEL_LOW)
    {
        pwm_leds_bitmask(0b001, 5);
    }
#ifdef CONFIG_VFX_LDO_PIN
    gpio_pin_set_dt(&ldo_pin, 0);
#endif
    if (fmt_state == IDLE && battery_level <= CONFIG_VFX_BATTERY_LEVEL_LOW)
    {
        atomic_inc(&anim.next_gen);
        fmt_event = START_DISCHARGED;
        k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
    }
}

#ifdef CONFIG_REPORT_ATTR
static void cpi_status_animation()
{
    uint16_t cpi = report.val;
#ifdef CONFIG_VFX_LDO_PIN
    gpio_pin_set_dt(&ldo_pin, 1);
#endif
    if (cpi > CONFIG_VFX_CPI_LEVEL_ULTRA)
    {
        pwm_leds_bitmask(0b101, 5);
    }
    else if (cpi > CONFIG_VFX_CPI_LEVEL_HIGH)
    {
        pwm_leds_bitmask(0b010, 5);
    }
    else if (cpi > CONFIG_VFX_CPI_LEVEL_MID)
    {
        pwm_leds_bitmask(0b011, 5);
    }
    else
    {
        pwm_leds_bitmask(0b001, 5);
    }
#ifdef CONFIG_VFX_LDO_PIN
    gpio_pin_set_dt(&ldo_pin, 0);
#endif
}

static void power_mode_animation()
{
    uint16_t power_mode = report.val;
#ifdef CONFIG_VFX_LDO_PIN
    gpio_pin_set_dt(&ldo_pin, 1);
#endif
    pwm_leds_bitmask(0b111, 3); // prefix animation
    switch (power_mode)
    {
    case 0:
        pwm_leds_bitmask(0b101, 3);
        break;
    case 1:
        pwm_leds_bitmask(0b011, 3);
        break;
    case 2:
        pwm_leds_bitmask(0b100, 3);
        break;
    default:
        pwm_leds_bitmask(0b001, 3);
        break;
    }
#ifdef CONFIG_VFX_LDO_PIN
    gpio_pin_set_dt(&ldo_pin, 0);
#endif
}

static void attr_animation()
{
    uint8_t attr = report.attr;
    switch (attr)
    {
    case 0:
        cpi_status_animation();
        break;
    case 1:
        power_mode_animation();
        break;
    default:
        break;
    }
}
#endif

static void discharged_animation()
{
#ifdef CONFIG_VFX_LDO_PIN
    gpio_pin_set_dt(&ldo_pin, 1);
#endif
    for (;;)
    {
        if (anim.current_gen != atomic_get(&anim.next_gen))
        {
#ifdef CONFIG_VFX_LDO_PIN
            gpio_pin_set_dt(&ldo_pin, 0);
#endif
            return;
        }
        pwm_leds_bitmask(0b001, 5);
    }
}

static void charging_animation()
{
#ifdef CONFIG_VFX_LDO_PIN
    gpio_pin_set_dt(&ldo_pin, 1);
#endif
    for (;;)
    {
        if (anim.current_gen != atomic_get(&anim.next_gen))
        {
#ifdef CONFIG_VFX_LDO_PIN
            gpio_pin_set_dt(&ldo_pin, 0);
#endif
            return;
        }
        pwm_leds_bitmask(0b011, 20);
    }
}

void handle_battery_status_event()
{
    switch (fmt_state)
    {
    case CHARGING:
        battery_status_animation();
        atomic_inc(&anim.next_gen);
        fmt_event = START_CHARGING;
        k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
        break;
    case DISCHARGED:
        battery_status_animation();
        atomic_inc(&anim.next_gen);
        fmt_event = START_DISCHARGED;
        k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
        break;
    case IDLE:
        battery_status_animation();
        break;
    default:
        break;
    }
}

void handle_connection_status_event()
{
    switch (fmt_state)
    {
    case CHARGING:
        battery_status_animation();
        atomic_inc(&anim.next_gen);
        fmt_event = START_CHARGING;
        k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
        break;
    case DISCHARGED:
        battery_status_animation();
        atomic_inc(&anim.next_gen);
        fmt_event = START_DISCHARGED;
        k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
        break;
    case IDLE:
        battery_status_animation();
        break;
    default:
        break;
    }
}

#ifdef CONFIG_REPORT_ATTR
void handle_attr_report_status_event()
{
    switch (fmt_state)
    {
    case CHARGING:
        attr_animation();
        atomic_inc(&anim.next_gen);
        fmt_event = START_CHARGING;
        k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
        break;
    case DISCHARGED:
        attr_animation();
        atomic_inc(&anim.next_gen);
        fmt_event = START_DISCHARGED;
        k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
        break;
    case IDLE:
        attr_animation();
        break;
    default:
        break;
    }
}
#endif

static void anim_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct anim_ctx *ctx = CONTAINER_OF(dwork, struct anim_ctx, work);
    ctx->current_gen = atomic_get(&ctx->next_gen);

    switch (fmt_event)
    {
    case START_BATTERY_STATUS:
        handle_battery_status_event();
        break;
    case START_CONNECTION_STATUS:
        handle_connection_status_event();
        break;
#ifdef CONFIG_REPORT_ATTR
    case START_ATTR_REPORT:
        handle_attr_report_status_event();
#endif
        break;
    case START_CHARGING:
        fmt_state = CHARGING;
        charging_animation();
        break;
    case START_DISCHARGED:
        fmt_state = DISCHARGED;
        discharged_animation();
        break;
    case STOP_ANIMATION:
        leds_off();
        fmt_state = IDLE;
        break;
    default:
        LOG_DBG("Unknown animation event: %d", fmt_event);
        break;
    }
}

// int conn_listener(const zmk_event_t *eh)
// {
//     indicate_connection();
//     return ZMK_EV_EVENT_BUBBLE;
// }
// ZMK_LISTENER(conn_state, conn_listener)
// #if IS_ENABLED(CONFIG_ZMK_BLE)
// ZMK_SUBSCRIPTION(conn_state, zmk_endpoint_changed);
// ZMK_SUBSCRIPTION(conn_state, zmk_ble_active_profile_changed);
// #endif

int battery_listener(const zmk_event_t *eh)
{
    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;

    if (fmt_state == IDLE && battery_level <= CONFIG_VFX_BATTERY_LEVEL_LOW)
    {
        atomic_inc(&anim.next_gen);
        fmt_event = START_DISCHARGED;
        k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(battery_state, battery_listener);
ZMK_SUBSCRIPTION(battery_state, zmk_battery_state_changed);

void indicate_battery()
{
    atomic_inc(&anim.next_gen);
    fmt_event = START_BATTERY_STATUS;
    k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
}

void indicate_connection()
{
    atomic_inc(&anim.next_gen);
    fmt_event = START_CONNECTION_STATUS;
    k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
}

#if IS_ENABLED(CONFIG_REPORT_ATTR)
void attr_report_consumer_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    
    while (1)
    {
        k_msgq_get(&attr_msgq, &report, K_FOREVER);
        atomic_inc(&anim.next_gen);
        fmt_event = START_ATTR_REPORT;
        k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
    }
}
#endif

#ifdef CONFIG_VFX_LDO_PIN
static int ldo_init()
{
    if (!gpio_is_ready_dt(&ldo_pin))
    {
        printk("LDO pin not ready\n");
        return -ENODEV;
    }

    // Configure as output, initially active (HIGH)
    int ret;
    ret = gpio_pin_configure_dt(&ldo_pin, GPIO_OUTPUT_INACTIVE);
    if (ret < 0)
    {
        printk("Failed to configure pin\n");
        return -ENODEV;
    }
    return 0;
}
#endif

#ifdef CONFIG_VFX_CHRG_PIN
static void chrg_changed(const struct device *dev,
                         struct gpio_callback *cb,
                         uint32_t pins)
{
    k_sleep(K_MSEC(10)); // Debounce delay
    bool charging = gpio_pin_get_dt(&chrg_pin);
    LOG_DBG("CHRG pin changed, charging: %d", charging);

    if (charging)
    {
        LOG_DBG("Charging started");
        atomic_inc(&anim.next_gen);
        fmt_event = START_CHARGING;
        k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
    }
    else
    {
        LOG_DBG("Charging finished");
        atomic_inc(&anim.next_gen);
        fmt_event = STOP_ANIMATION;
        k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);
    }
}

static int chrg_init()
{
    if (!gpio_is_ready_dt(&chrg_pin))
    {
        printk("CHRG pin not ready\n");
        return -ENODEV;
    }

    // Configure as input with pull-up
    int ret;
    ret = gpio_pin_configure_dt(&chrg_pin, GPIO_INPUT | GPIO_PULL_UP);
    if (ret < 0)
    {
        printk("Failed to configure pin\n");
        return -ENODEV;
    }

    ret = gpio_pin_interrupt_configure_dt(
        &chrg_pin,
        GPIO_INT_EDGE_BOTH);
    if (ret)
    {
        return ret;
    }

    gpio_init_callback(&chrg_cb,
                       chrg_changed,
                       BIT(chrg_pin.pin));

    gpio_add_callback(chrg_pin.port, &chrg_cb);

    return 0;
}
#endif

static int init_animation(void)
{
    k_work_queue_init(&animation_work_q);

    k_work_queue_start(&animation_work_q, animation_work_q_stack,
                       K_THREAD_STACK_SIZEOF(animation_work_q_stack), K_LOWEST_APPLICATION_THREAD_PRIO,
                       NULL);

#ifdef CONFIG_VFX_LDO_PIN
    ldo_init(&ldo_pin);
#endif

    atomic_set(&anim.next_gen, 0);
    anim.current_gen = 0;
    fmt_state = IDLE;
    fmt_event = STOP_ANIMATION;
    k_work_init_delayable(&anim.work, anim_handler);
    k_work_schedule_for_queue(&animation_work_q, &anim.work, K_NO_WAIT);

#if IS_ENABLED(CONFIG_REPORT_ATTR)
    k_thread_create(&msgq_thread_data, msgq_thread_stack,
                    K_THREAD_STACK_SIZEOF(msgq_thread_stack),
                    attr_report_consumer_thread,
                    NULL, NULL, NULL,
                    K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
#endif

#ifdef CONFIG_VFX_CHRG_PIN
    chrg_init(&chrg_pin);

    bool charging = gpio_pin_get_dt(&chrg_pin);
    if (charging)
    {
        atomic_inc(&anim.next_gen);
        fmt_event = START_CHARGING;
        k_work_reschedule_for_queue(&animation_work_q, &anim.work, K_MSEC(1500));
    }
#endif

    return 0;
}

SYS_INIT(init_animation, APPLICATION, 32);