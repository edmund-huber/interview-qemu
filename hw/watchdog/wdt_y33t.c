#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "sysemu/watchdog.h"

#define TYPE_WDT_Y33T "y33t"
#define WDT_Y33T(obj) OBJECT_CHECK(Y33TState, (obj), TYPE_WDT_Y33T)

#define Y33T_MEM_SIZE 2

typedef enum {
    Y33T_BOOT_CLEAN = 0,
    Y33T_BOOT_NOPING = 1,
    Y33T_BOOT_OVERHEAT = 2,
    Y33T_BOOT_OTHER = 0xff
} y33t_boot_reason;

typedef struct Y33TState {
    I2CSlave parent_obj;

    bool addr_byte;
    uint8_t ptr;
    QEMUTimer *timer;
    bool armed;
    y33t_boot_reason boot_reason;
} Y33TState;

static const VMStateDescription vmstate_y33t = {
    .name = "vmstate_y33t",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (VMStateField[]) {
        VMSTATE_BOOL(addr_byte, Y33TState),
        VMSTATE_UINT8(ptr, Y33TState),
        VMSTATE_TIMER_PTR(timer, Y33TState),
        VMSTATE_BOOL(armed, Y33TState),
        VMSTATE_END_OF_LIST()
    }
};

static y33t_boot_reason this_boot = Y33T_BOOT_CLEAN;

static void wdt_y33t_reset(DeviceState *dev)
{
    Y33TState *y33t = WDT_Y33T(dev);

    y33t->addr_byte = false;
    y33t->ptr = 0;
    timer_del(y33t->timer);
    y33t->armed = false;
    y33t->boot_reason = this_boot;
    this_boot = Y33T_BOOT_CLEAN;
}

static void y33t_timer_expired(void *dev)
{
    Y33TState *y33t = WDT_Y33T(dev);

    if (y33t->armed) {
        this_boot = Y33T_BOOT_NOPING;
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        // TODO: see diag288, should be using the watchdog core API e.g.:
        // watchdog_perform_action();
    }
}

static int wdt_y33t_event(I2CSlave *i2c, enum i2c_event event)
{
    Y33TState *y33t = WDT_Y33T(i2c);

    if (event == I2C_START_SEND) {
        y33t->addr_byte = true;
    }

    return 0;
}

static uint8_t wdt_y33t_recv(I2CSlave *i2c)
{
    Y33TState *y33t = WDT_Y33T(i2c);
    uint8_t res;
    switch (y33t->ptr) {
    case 0:
        res = y33t->armed ? 1 : 0;
        break;
    case 1:
        res = y33t->boot_reason;
        break;
    default:
        res = 0xff;
        break;
    }
    y33t->ptr = (y33t->ptr + 1) % Y33T_MEM_SIZE;
    return res;
}

static int wdt_y33t_send(I2CSlave *i2c, uint8_t data)
{
    Y33TState *y33t = WDT_Y33T(i2c);

    if (y33t->addr_byte) {
        y33t->ptr = data < Y33T_MEM_SIZE ? data : 0;
        y33t->addr_byte = false;
        return 0;
    }

    if (y33t->ptr == 0) {
        switch (data) {
        case 13:
            y33t->armed = true;
            timer_mod(
                y33t->timer,
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (3 * NANOSECONDS_PER_SECOND)
            );
            break;
        }
    }

    return 0;
}

static void wdt_y33t_realize(DeviceState *dev, Error **errp)
{
    Y33TState *y33t = WDT_Y33T(dev);

    y33t->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, y33t_timer_expired, dev);
}

static void wdt_y33t_unrealize(DeviceState *dev, Error **errp)
{
    Y33TState *y33t = WDT_Y33T(dev);

    timer_del(y33t->timer);
    timer_free(y33t->timer);
}

static void wdt_y33t_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = wdt_y33t_event;
    k->recv = wdt_y33t_recv;
    k->send = wdt_y33t_send;

    dc->realize = wdt_y33t_realize;
    dc->unrealize = wdt_y33t_unrealize;
    dc->reset = wdt_y33t_reset;
    dc->vmsd = &vmstate_y33t;
}

static const TypeInfo wdt_y33t_info = {
    .class_init = wdt_y33t_class_init,
    .parent = TYPE_I2C_SLAVE,
    .name  = TYPE_WDT_Y33T,
    .instance_size = sizeof(Y33TState),
};

static WatchdogTimerModel model = {
    .wdt_name = TYPE_WDT_Y33T,
    .wdt_description = "y33t",
};

static void wdt_y33t_register_types(void)
{
    watchdog_add_model(&model);
    type_register_static(&wdt_y33t_info);
}

type_init(wdt_y33t_register_types)
