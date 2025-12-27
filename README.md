# VFX RGBLED indicator

ZMK module for showing battery and connection status by led animation

## Features
* 
* three leds for animations
* pwm support for smooth animation
* cpi indication of mouse

### Battery animation
Battery animation can show different states of battery level which can be configured through following options:
* `CONFIG_VFX_INDICATOR_BATTERY_LEVEL_HIGH`
* `CONFIG_VFX_INDICATOR_BATTERY_LEVEL_MID`
* `CONFIG_VFX_INDICATOR_BATTERY_LEVEL_LOW`
* `CONFIG_VFX_INDICATOR_BATTERY_LEVEL_CRITICAL`

### CPI animation
CPI animation can show different states of cpi level which can be configured through following options:
* `CONFIG_VFX_CPI_LEVEL_ULTRA` - at all levels above this the LED will glow purple
* `CONFIG_VFX_CPI_LEVEL_HIGH` - at all levels above this the LED will glow purple
* `CONFIG_VFX_CPI_LEVEL_MID` - at all levels above this the LED will glow purple
* At all levels below `CONFIG_VFX_CPI_LEVEL_MID` LED will glow red
To enable cpi animation your sensor driver should add `CONFIG_REPORT_CPI`.

## Installation
To use, first add this module to your `config/west.yml` by adding a new entry to `remotes` and `projects`:

```yaml west.yml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: ggrocer  # <-- new entry
      url-base: https://github.com/greengrocer98
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-vfx-rgbled-indicator  # <-- new entry
      remote: ggrocer
      revision: main
  self:
    path: config
```

For more information, including instructions for building locally, check out the ZMK docs on [building with modules](https://zmk.dev/docs/features/modules#building-with-modules).

Then, add support of module in `<keyboard_or_mouse>.conf` by adding `CONFIG_VFX_RGBLED_INDICATOR=y`.
Add custom behaviors for showing battery and connection status by pressing buttons in `<keyboard_or_mouse>.keymap`:

```dts
#include <behaviors/vfx_indicator.dtsi>  // needed to use the behaviors

/ {
    keymap {
        ...
        some_layer {
            bindings = <
                ...
                &check_bat   // check battery level
                &check_conn  // check connection status
                ...
            >;
        };
    };
};
```

And finally configure led pins in `<keyboard_or_mouse>.overlay`:

```overlay
/ {
    backlight: pwmleds {
        compatible = "pwm-leds";
        pwm_led_0: pwm_led_0 {
            pwms = <&pwm0 0 PWM_MSEC(10) PWM_POLARITY_NORMAL>;
        };
        pwm_led_1: pwm_led_1 {
            pwms = <&pwm0 1 PWM_MSEC(10) PWM_POLARITY_NORMAL>;
        };
        pwm_led_2: pwm_led_2 {
            pwms = <&pwm0 2 PWM_MSEC(10) PWM_POLARITY_NORMAL>;
        };
    };

    aliases {
        led0 = &pwm_led_0;
        led1 = &pwm_led_1;
        led2 = &pwm_led_2;
    };
};

&pinctrl {
    pwm0_default: pwm0_default {
        group1 {
            psels = <NRF_PSEL(PWM_OUT0, 0, 31)>, // LED RED
                    <NRF_PSEL(PWM_OUT1, 0, 29)>, // LED GREEN
                    <NRF_PSEL(PWM_OUT2, 0, 2)>;  // LED BLUE
        };
    };
    pwm0_sleep: pwm0_sleep {
        group1 {
            psels = <NRF_PSEL(PWM_OUT0, 0, 31)>, // LED RED
                    <NRF_PSEL(PWM_OUT1, 0, 29)>, // LED GREEN
                    <NRF_PSEL(PWM_OUT2, 0, 2)>;  // LED BLUE
            low-power-enable;
        };
    };
};

&pwm0 {
    status = "okay";
    pinctrl-0 = <&pwm0_default>;
    pinctrl-1 = <&pwm0_sleep>;
    pinctrl-names = "default", "sleep";
};
```

If your LEDs are powered by LDO, you should add LDO pin. If your keyboard or mouse also has CHARGER pin that shows the charging status you also should add it.
```
/ {
  	additional_gpio: additional_gpio {
  		compatible = "gpio-leds";
  		ldo_gpio: ldo_gpio {
  			gpios = <&gpio1 02 GPIO_ACTIVE_HIGH>;
  		};
  		chrg_gpio: chrg_gpio {
  			gpios = <&gpio0 26 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
  		};
  	};

    aliases {
    		ldo  = &ldo_gpio;
    		chrg = &chrg_gpio;
    };
};
```
and enable `CONFIG_VFX_LDO_PIN=y` and `CONFIG_VFX_CHRG_PIN=y`.

## References
This module was inspired by
* [zmk-rgbled-widget](https://github.com/caksoylar/zmk-rgbled-widget/) by [caksoylar](https://github.com/caksoylar)
* [status_led](https://github.com/aroum/zmk-kabarga/blob/kabarga/config/boards/shields/kabarga/status_led.c) by [aroum](https://github.com/aroum)
